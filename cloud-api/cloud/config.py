import os
from typing import Optional

from google.auth import default as google_auth_default


def _detect_project_id() -> Optional[str]:
    env_project_id = os.environ.get("GOOGLE_CLOUD_PROJECT_ID", os.environ.get("GCP_PROJECT_ID"))
    if env_project_id:
        return env_project_id
    try:
        _, detected_project_id = google_auth_default()
        return detected_project_id
    except Exception:
        return None


GOOGLE_CLOUD_PROJECT_ID = _detect_project_id()
VERTEX_AI_LOCATION = os.environ.get("VERTEX_AI_LOCATION", "us-central1")

AUDIO_HP_CUTOFF_HZ = int(os.environ.get("AUDIO_HP_CUTOFF_HZ", "220"))
AUDIO_LP_CUTOFF_HZ = int(os.environ.get("AUDIO_LP_CUTOFF_HZ", "3400"))
AUDIO_TARGET_DBFS = float(os.environ.get("AUDIO_TARGET_DBFS", "-24.0"))
AUDIO_MAX_GAIN_DB = float(os.environ.get("AUDIO_MAX_GAIN_DB", "10.0"))
TTS_VOICE_NAME = os.environ.get("TTS_VOICE_NAME", "id-ID-Wavenet-A")  # A = female

AUDIO_CACHE_MAX_ITEMS = int(os.environ.get("AUDIO_CACHE_MAX_ITEMS", "20"))
DB_FILENAME = os.environ.get("DB_FILENAME", "session_history.db")
