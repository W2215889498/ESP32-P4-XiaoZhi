"""Xiaozhi wire protocol: message builders and constants.

Frame layout
------------
* Text frame : JSON control message (``type`` field)
* Binary frame: one Opus packet of microphone audio (device -> server)
                or TTS audio (server -> device)
"""

from __future__ import annotations

import json
import time
from typing import Any


# ----- device -> server message types -----
TYPE_HELLO = "hello"
TYPE_LISTEN = "listen"
TYPE_ABORT = "abort"
TYPE_MCP = "mcp"

# listen states
LISTEN_START = "start"
LISTEN_STOP = "stop"
LISTEN_DETECT = "detect"

# listen modes
MODE_AUTO = "auto"
MODE_MANUAL = "manual"
MODE_REALTIME = "realtime"

# tts states
TTS_START = "start"
TTS_STOP = "stop"
TTS_SENTENCE_START = "sentence_start"


def dumps(obj: dict[str, Any]) -> str:
    return json.dumps(obj, ensure_ascii=False)


def loads(text: str) -> dict[str, Any]:
    return json.loads(text)


def hello_ack(session_id: str, audio_params: dict[str, Any]) -> dict[str, Any]:
    return {
        "type": TYPE_HELLO,
        "session_id": session_id,
        "transport": "websocket",
        "audio_params": audio_params,
    }


def stt(text: str) -> dict[str, Any]:
    """Recognised user speech (ASR result)."""
    return {"type": "stt", "text": text}


def tts(state: str, text: str | None = None) -> dict[str, Any]:
    msg: dict[str, Any] = {"type": "tts", "state": state}
    if text is not None:
        msg["text"] = text
    return msg


def emotion(name: str) -> dict[str, Any]:
    return {"type": "llm", "emotion": name}


def mcp(payload: dict[str, Any]) -> dict[str, Any]:
    return {"type": TYPE_MCP, "payload": payload}


def server_time() -> dict[str, Any]:
    """Server time block returned by the config endpoint."""
    return {
        "timestamp": int(time.time() * 1000),
        "timezone_offset": 8 * 60,  # minutes east of UTC
    }
