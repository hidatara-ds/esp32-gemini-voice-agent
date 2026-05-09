from . import state
from .app_factory import create_app

app, socketio = create_app()

# Compatibility exports
SESSIONS = state.SESSIONS
SID_TO_SESSION = state.SID_TO_SESSION
