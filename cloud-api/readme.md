# Gabriel AI — Voice Assistant Backend

Real-time AI voice assistant powered by **Gemini 2.5 Flash**, **Google Cloud Speech-to-Text**, and **Google Cloud Text-to-Speech**. Supports **WebSocket real-time audio streaming** and **REST API** for ESP32-S3 firmware integration.

---

## Architecture

```
ESP32-S3 (I2S Mic)
    │
    ├── REST POST /api/process-audio  (Base64 WAV)
    └── WebSocket  /socket.io         (raw PCM16 stream)
            │
            ▼
    Gabriel Cloud Run (Flask + Flask-SocketIO)
            │
            ├── Google Cloud Speech-to-Text (STT)
            ├── Gemini 2.5 Flash (AI response)
            └── Google Cloud Text-to-Speech (TTS)
                        │
                        ▼
            Response: text answer + audio URL
```

---

## File Structure

```
gabriel/
├── cloud/                      # Cloud Run app module
│   ├── __init__.py
│   ├── app_factory.py          # Flask + SocketIO factory
│   ├── config.py               # Environment config
│   ├── db.py                   # SQLite chat history
│   ├── main.py                 # Entry point (cloud.main:app)
│   ├── routes.py               # HTTP REST endpoints
│   ├── services.py             # STT, Gemini, TTS logic
│   ├── state.py                # In-memory state (sessions, cache)
│   └── websocket_handlers.py   # Socket.IO event handlers
│
├── static/                     # Frontend static files (served by Flask)
│   ├── index.html              # Premium dark UI (WebSocket test, API test, ESP32 guide)
│   ├── app.js                  # WebSocket state machine frontend
│   └── styles.css              # Dark navy design system
│
├── app.py                      # Legacy monolith (reference only)
├── Dockerfile                  # Cloud Run container config
├── Procfile                    # Gunicorn entrypoint
├── requirements.txt            # Python dependencies
├── session_history.db          # SQLite chat history (ephemeral in Cloud Run)
└── readme.md                   # This file
```

---

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/` | Landing page & WebSocket test UI |
| `GET` | `/api/health` | Health check (Cloud Run probe) |
| `POST` | `/api/process-audio` | Audio (Base64 WAV) → STT → Gemini → TTS |
| `POST` | `/api/test-text` | Text prompt → Gemini response (no audio) |
| `GET` | `/api/audio/<id>` | Cached TTS audio playback |
| `GET` | `/api/debug/latest-input-meta` | Metadata audio input terakhir |
| `GET` | `/api/debug/latest-input-wav` | Download WAV input terakhir |

---

## WebSocket Events

**Server: `wss://YOUR-URL/socket.io/`**

### Client → Server (emit `message`)

```json
// Ping keepalive
{"type": "ping"}

// Signal end of audio stream (trigger STT processing)
{"type": "audio_end"}

// Binary audio chunks (ArrayBuffer, max 5120 bytes each)
<binary>
```

### Server → Client (on `message`)

```json
{"type": "connected",  "session_id": "uuid"}         // on connect
{"type": "pong"}                                       // ping response
{"type": "processing"}                                 // STT in progress
{"type": "stt_result", "text": "..."}                 // STT result
{"type": "result",     "question": "...",              // full AI response
                       "answer": "...",
                       "audio_url": "/api/audio/...",
                       "audio_id": "...",
                       "audio_mime": "audio/mpeg"}
{"type": "error",      "message": "..."}              // error
```

---

## ESP32-S3 Integration

### Option 1: REST API (Simpler)

```cpp
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Base64.h>

const char* SERVER_URL = "https://YOUR-SERVICE.run.app/api/process-audio";

void sendAudio(uint8_t* pcmBuf, size_t len) {
  String b64 = base64::encode(pcmBuf, len);

  StaticJsonDocument<512> doc;
  doc["audio"]          = b64;
  doc["session_id"]     = "esp32-001";
  doc["include_audio"]  = true;
  doc["audio_delivery"] = "url";   // returns audio_url to fetch
  String payload; serializeJson(doc, payload);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Client", "esp32");   // minimal response payload
  int code = http.POST(payload);

  if (code == 200) {
    String resp = http.getString();
    // resp: {"question":"...","answer":"...","audio_url":"/api/audio/..."}
  }
  http.end();
}
```

### Option 2: WebSocket Streaming (Lower Latency)

```cpp
#include <WebSocketsClient.h>

WebSocketsClient ws;

void setup() {
  ws.beginSSL("YOUR-SERVICE.run.app", 443,
              "/socket.io/?transport=websocket&EIO=4");
  ws.onEvent(wsEvent);
}

void wsEvent(WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_TEXT) {
    // Parse: {"type":"result","question":"...","answer":"...","audio_url":"..."}
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload, len);
    if (String(doc["type"]) == "result") {
      String answer = doc["answer"];
      String audioUrl = doc["audio_url"];
      // Display on OLED, fetch and play TTS audio...
    }
  }
}

void sendAudioStream(uint8_t* pcm, size_t len) {
  // Send in chunks
  for (size_t i = 0; i < len; i += 5120) {
    size_t chunk = min((size_t)5120, len - i);
    ws.sendBIN(pcm + i, chunk);
  }
  // Signal end
  ws.sendTXT("{\"type\":\"audio_end\"}");
}
```

### Audio Format Requirements

| Parameter | Value |
|-----------|-------|
| Format | PCM16 raw OR WAV |
| Sample Rate | **16000 Hz** |
| Channels | **1 (Mono)** |
| Bit Depth | **16-bit** |
| Max Duration | 10 seconds (320KB) |
| Language | Indonesian (`id-ID`) |

---

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `GOOGLE_CLOUD_PROJECT_ID` | ✅ | GCP project ID |
| `VERTEX_AI_LOCATION` | optional | Default: `us-central1` |
| `AUDIO_HP_CUTOFF_HZ` | optional | High-pass filter (default: 220Hz) |
| `AUDIO_LP_CUTOFF_HZ` | optional | Low-pass filter (default: 3400Hz) |
| `AUDIO_TARGET_DBFS` | optional | Target gain level (default: -24.0) |
| `AUDIO_MAX_GAIN_DB` | optional | Max gain applied (default: 10.0) |

---

## Deployment (Cloud Run)

```bash
# Build & deploy via Cloud Build (auto-triggered on git push)
git add -A && git commit -m "deploy" && git push origin main

# Or manually:
gcloud builds submit --tag gcr.io/PROJECT_ID/gabriel .
gcloud run deploy gabriel \
  --image gcr.io/PROJECT_ID/gabriel \
  --platform managed \
  --region us-central1 \
  --allow-unauthenticated \
  --set-env-vars GOOGLE_CLOUD_PROJECT_ID=PROJECT_ID
```

---

## Local Development

```bash
pip install -r requirements.txt
export GOOGLE_CLOUD_PROJECT_ID=your-project-id
python -m cloud.main   # atau
gunicorn --bind 0.0.0.0:8080 --worker-class eventlet --workers 1 cloud.main:app
```

Open: `http://localhost:8080`

---

## Notes

- **Session history** (`session_history.db`) is not persistent in Cloud Run — each instance starts fresh. For production, use Cloud SQL or Firestore.
- **Concurrency**: Cloud Run is configured with 1 worker (eventlet) to avoid WebSocket state conflicts. Scale via multiple instances (each has its own session store).
- **ESP32 Client Detection**: Set `X-Client: esp32` header OR use `ESP32HTTPClient` user-agent to receive minimal JSON payload (reduces firmware memory pressure).
