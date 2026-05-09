from typing import Dict, Any


LATEST_INPUT_WAV = None
LATEST_INPUT_META = {}
AUDIO_CACHE: Dict[str, Dict[str, Any]] = {}

SESSIONS: Dict[str, Dict[str, Any]] = {}
SID_TO_SESSION: Dict[str, str] = {}

NOTES: Dict[str, Dict[str, str]] = {}   # session_id → {key: value}
