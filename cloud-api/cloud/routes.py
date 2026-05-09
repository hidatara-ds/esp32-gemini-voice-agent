import base64
import os
from datetime import datetime, timezone
import uuid

from flask import request, jsonify, Response, send_from_directory

from . import state
from .services import (
    decode_and_convert_audio,
    stt_transcribe,
    ask_gemini,
    synthesize_speech_bytes,
    cache_audio_bytes,
    detect_language,
)

# Path to static files (sibling of cloud/ in project root)
_STATIC_DIR = os.path.join(os.path.dirname(__file__), '..', 'static')


def register_routes(app):
    @app.route("/")
    def index():
        """Serve premium landing page — NOT JSON."""
        return send_from_directory(_STATIC_DIR, 'index.html')

    @app.route("/styles.css")
    def styles():
        return send_from_directory(_STATIC_DIR, 'styles.css')

    @app.route("/app.js")
    def appjs():
        return send_from_directory(_STATIC_DIR, 'app.js')

    @app.route("/api/health", methods=["GET"])
    def health():
        """Health check for Cloud Run liveness probe."""
        return jsonify({"status": "ok", "service": "gabriel-cloud-backend", "version": "2.0"})

    @app.route("/api/test-text", methods=["POST"])
    def test_text():
        """Test Gemini without audio — accepts plain text, returns AI response."""
        try:
            data = request.get_json(silent=True) or {}
            text = data.get("text", "").strip()
            if not text:
                return jsonify({"error": "Field 'text' required"}), 400
            session_id = data.get("session_id") or uuid.uuid4().hex
            diagnostics = {"transport": "rest_text"}
            answer = ask_gemini(session_id, text, diagnostics)
            return jsonify({
                "question": text,
                "answer": answer,
                "session_id": session_id,
                "stage": "ok",
            })
        except Exception as exc:
            return jsonify({"error": str(exc), "stage": "internal_error"}), 500

    @app.route("/api/process-audio", methods=["POST"])
    def api_process_audio():
        diagnostics = {
            "content_type": request.content_type,
            "content_length": request.content_length,
            "is_json": request.is_json,
            "user_agent": request.user_agent.string if request.user_agent else None,
        }
        try:
            if not request.is_json:
                return jsonify({"error": "Content-Type harus application/json", "stage": "validate_content_type", "diagnostics": diagnostics}), 400

            data = request.get_json()
            if not isinstance(data, dict):
                return jsonify({"error": "Body JSON tidak valid", "stage": "parse_json", "diagnostics": diagnostics}), 400

            audio_data = data.get("audio")
            session_id = data.get("session_id") or uuid.uuid4().hex
            diagnostics["session_id"] = session_id
            diagnostics["audio_b64_len"] = len(audio_data) if isinstance(audio_data, str) else 0

            if not audio_data:
                return jsonify({"error": "Audio data tidak ditemukan", "stage": "validate_audio_field", "diagnostics": diagnostics}), 400

            try:
                converted_bytes = decode_and_convert_audio(audio_data, diagnostics)
            except Exception as exc:
                return jsonify({"error": f"Audio base64/convert tidak valid: {str(exc)}", "stage": "decode_or_convert", "diagnostics": diagnostics}), 400

            state.LATEST_INPUT_WAV = converted_bytes
            state.LATEST_INPUT_META = {
                "session_id": session_id,
                "bytes": len(converted_bytes),
                "sample_rate": 16000,
                "channels": 1,
                "sample_width": 2,
                "timestamp": datetime.now(timezone.utc).isoformat(),
            }

            question = stt_transcribe(converted_bytes, diagnostics)
            if not question:
                return jsonify({"error": "Tidak ada teks yang terdeteksi", "stage": "stt_empty_result", "session_id": session_id, "diagnostics": diagnostics}), 400

            lang   = detect_language(question)
            diagnostics["detected_lang"] = lang
            answer = ask_gemini(session_id, question, diagnostics)
            response_payload = {
                "question": question,
                "answer": answer,
                "session_id": session_id,
                "stage": "ok",
                "diagnostics": diagnostics,
            }

            user_agent = (request.user_agent.string or "") if request.user_agent else ""
            is_esp_client = ("ESP32HTTPClient" in user_agent) or (request.headers.get("X-Client", "").lower() == "esp32")
            include_audio_req = data.get("include_audio")
            include_audio = (not is_esp_client) if include_audio_req is None else bool(include_audio_req)
            audio_delivery = (data.get("audio_delivery") or "auto").lower()

            if audio_delivery == "auto":
                if not include_audio:
                    audio_delivery = "none"
                else:
                    audio_delivery = "url" if is_esp_client else "inline"

            diagnostics["is_esp_client"] = is_esp_client
            diagnostics["include_audio"] = include_audio
            diagnostics["audio_delivery"] = audio_delivery

            if include_audio and audio_delivery in ("inline", "url"):
                tts_text = answer.strip() if answer.strip() else "Maaf."
                audio_bytes = synthesize_speech_bytes(tts_text, language=lang)
                if audio_delivery == "inline":
                    response_payload["audio_base64"] = base64.b64encode(audio_bytes).decode("utf-8")
                else:
                    audio_id, audio_url = cache_audio_bytes(audio_bytes)
                    response_payload["audio_id"] = audio_id
                    response_payload["audio_url"] = audio_url
                    response_payload["audio_mime"] = "audio/wav"

            if is_esp_client:
                minimal_payload = {
                    "question": question,
                    "answer": answer,
                    "session_id": session_id,
                    "stage": "ok",
                }
                if "audio_url" in response_payload:
                    minimal_payload["audio_url"] = response_payload["audio_url"]
                    minimal_payload["audio_id"] = response_payload.get("audio_id")
                    minimal_payload["audio_mime"] = response_payload.get("audio_mime")
                return jsonify(minimal_payload)

            return jsonify(response_payload)
        except Exception as exc:
            return jsonify({"error": str(exc), "stage": "internal_error", "diagnostics": diagnostics}), 500

    @app.route("/api/audio/<audio_id>", methods=["GET"])
    def get_cached_audio(audio_id):
        audio_item = state.AUDIO_CACHE.get(audio_id)
        if not audio_item:
            return jsonify({"error": "Audio tidak ditemukan atau kadaluarsa"}), 404
        return Response(
            audio_item["bytes"],
            mimetype=audio_item.get("mime", "audio/mpeg"),
            headers={"Content-Disposition": f"inline; filename={audio_id}.mp3"},
        )

    @app.route("/api/debug/latest-input-meta", methods=["GET"])
    def debug_latest_input_meta():
        if not state.LATEST_INPUT_META:
            return jsonify({"error": "Belum ada audio input tersimpan"}), 404
        return jsonify({"status": "ok", "meta": state.LATEST_INPUT_META})

    @app.route("/api/debug/latest-input-wav", methods=["GET"])
    def debug_latest_input_wav():
        if not state.LATEST_INPUT_WAV:
            return jsonify({"error": "Belum ada audio input tersimpan"}), 404
        return Response(
            state.LATEST_INPUT_WAV,
            mimetype="audio/wav",
            headers={"Content-Disposition": "inline; filename=latest-input.wav"},
        )
