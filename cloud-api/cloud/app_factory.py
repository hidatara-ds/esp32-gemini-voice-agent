from flask import Flask
from flask_socketio import SocketIO

from .db import init_db
from .routes import register_routes
from .websocket_handlers import register_websocket_handlers


def create_app():
    app = Flask(__name__)

    # threading async_mode = most compatible with Google Cloud gRPC libraries
    # eventlet/gevent conflict with gRPC → causes blocking even with monkey_patch
    socketio = SocketIO(
        app,
        cors_allowed_origins="*",
        async_mode="threading",
        ping_timeout=120,        # 120s — gives plenty of time for STT+AI+TTS
        ping_interval=30,        # send ping every 30s
        max_http_buffer_size=5 * 1024 * 1024,  # 5MB max payload
        logger=False,
        engineio_logger=False,
    )

    init_db()
    register_routes(app)
    register_websocket_handlers(socketio)
    return app, socketio
