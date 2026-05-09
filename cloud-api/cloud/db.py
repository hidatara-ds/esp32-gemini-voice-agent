import os
import sqlite3
from datetime import datetime

from .config import DB_FILENAME


DB_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)), DB_FILENAME)


def init_db() -> None:
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS chat_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id TEXT,
            role TEXT,
            message TEXT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        )
        """
    )
    conn.commit()
    conn.close()


def save_message(session_id: str, role: str, message: str) -> None:
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute(
        "INSERT INTO chat_history (session_id, role, message, timestamp) VALUES (?, ?, ?, ?)",
        (session_id, role, message, datetime.now()),
    )
    conn.commit()
    conn.close()


def get_history(session_id: str):
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute(
        "SELECT role, message FROM chat_history WHERE session_id = ? ORDER BY id ASC",
        (session_id,),
    )
    rows = cur.fetchall()
    conn.close()
    return [{"role": role, "parts": [message]} for role, message in rows]
