"""FastAPI entrypoint for the Xiaozhi protocol server (方案 A).

Endpoints
---------
GET  /                      status page
GET  /health                health + provider/codec report
GET/POST /xiaozhi/ota/      device discovery/config (returns the WS url)
WS   /xiaozhi/v1/           the Xiaozhi bidirectional streaming protocol
"""

from __future__ import annotations

import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

from . import __version__, protocol
from .audio import opus_available, opus_error
from .config import settings
from .session import ChatSession

log = logging.getLogger("xz.main")


def _check_runtime_env() -> None:
    """启动自检：解释器是否为项目 venv、所选 provider 的依赖是否齐全。"""
    import importlib.util
    import sys

    if ".venv" not in sys.executable.replace("\\", "/"):
        log.warning(
            "当前 Python 不是项目 venv（%s）！建议用 server\\run.ps1（或双击 run.bat）启动，否则可能缺依赖。",
            sys.executable,
        )

    def missing(mod: str) -> bool:
        return importlib.util.find_spec(mod) is None

    if settings.tts_provider == "edge" and missing("edge_tts"):
        log.warning("edge-tts 未安装：edge 语音合成会失败（pip install edge-tts）")
    if missing("miniaudio"):
        log.warning("miniaudio 未安装：MP3 等压缩音频解码会失败（edge/云 TTS 输出依赖它）")
    if settings.asr_provider in ("local", "whisper", "faster-whisper") and missing("faster_whisper"):
        log.warning("faster-whisper 未安装：本机 ASR 不可用")


@asynccontextmanager
async def lifespan(_: FastAPI):
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    log.info("xiaozhi-server %s", __version__)
    log.info("LLM : provider=%s model=%s key=%s", settings.llm_provider, settings.llm_model,
             "set" if settings.llm_api_key else "MISSING")
    log.info("ASR : provider=%s model=%s key=%s", settings.asr_provider, settings.asr_model,
             "set" if settings.asr_api_key else "MISSING")
    log.info("TTS : provider=%s voice=%s", settings.tts_provider, settings.tts_voice)
    if opus_available():
        log.info("audio: libopus OK (Opus 16k/mono/60ms)")
    else:
        log.error("audio: libopus MISSING (%s) - device audio will not work", opus_error())
    _check_runtime_env()

    log.info("listening on http://%s:%d  (ws path /xiaozhi/v1/)", settings.host, settings.port)
    yield


app = FastAPI(title="Xiaozhi Server", version=__version__, lifespan=lifespan)


def _ws_url(request: Request) -> str:
    if settings.public_ws_url:
        return settings.public_ws_url
    host = request.headers.get("host")
    if not host:
        host = f"{request.url.hostname or '127.0.0.1'}:{settings.port}"
    return f"ws://{host}/xiaozhi/v1/"


def _config_payload(request: Request) -> dict:
    """Reply for the device's OTA/config request; tells it where to connect."""
    return {
        "server_time": protocol.server_time(),
        "firmware": {"version": "0.0.0", "url": ""},
        "websocket": {"url": _ws_url(request), "token": ""},
    }


@app.get("/", response_class=HTMLResponse)
async def index(request: Request) -> str:
    ws = _ws_url(request)
    return f"""<!doctype html><html lang="zh"><meta charset="utf-8">
<title>xiaozhi-server</title>
<body style="font-family:system-ui;max-width:640px;margin:40px auto;line-height:1.7">
<h2>小智服务端 v{__version__}</h2>
<ul>
  <li>WebSocket: <code>{ws}</code></li>
  <li>配置端点: <code>{request.url.hostname or 'host'}/xiaozhi/ota/</code></li>
  <li>LLM: <b>{settings.llm_provider}</b> / {settings.llm_model}
      ({'key set' if settings.llm_api_key else '<span style="color:#c00">no key</span>'})</li>
  <li>ASR: <b>{settings.asr_provider}</b> / {settings.asr_model}</li>
  <li>TTS: <b>{settings.tts_provider}</b> / {settings.tts_voice}</li>
</ul>
<p>把设备指向 <code>{ws}</code> 即可。</p>
</body></html>"""


@app.get("/health")
async def health() -> dict:
    return {
        "status": "ok",
        "version": __version__,
        "opus": opus_available(),
        "opus_error": opus_error(),
        "llm": {"provider": settings.llm_provider, "model": settings.llm_model,
                "configured": bool(settings.llm_api_key)},
        "asr": {"provider": settings.asr_provider, "model": settings.asr_model,
                "configured": settings.asr_provider in ("local", "voicestudio", "whisper-api", "partner")
                              or bool(settings.asr_api_key)},
        "tts": {"provider": settings.tts_provider, "voice": settings.tts_voice},
    }


@app.api_route("/xiaozhi/ota/", methods=["GET", "POST"])
async def ota(request: Request) -> dict:
    payload = _config_payload(request)
    log.info("config request from %s -> %s", request.client.host if request.client else "?",
             payload["websocket"]["url"])
    return payload


@app.websocket("/xiaozhi/v1/")
async def xiaozhi_ws(ws: WebSocket) -> None:
    await ws.accept()
    remote = f"{ws.client.host}:{ws.client.port}" if ws.client else "unknown"
    log.info("[%s] connected", remote)
    session = ChatSession(ws, remote)
    try:
        while True:
            message = await ws.receive()
            if message.get("type") == "websocket.disconnect":
                break
            if message.get("text") is not None:
                await session.on_text(message["text"])
            elif message.get("bytes") is not None:
                await session.on_bytes(message["bytes"])
    except WebSocketDisconnect:
        pass
    except Exception:
        log.exception("[%s] websocket error", remote)
    finally:
        await session.close()
        log.info("[%s] disconnected", remote)


def main() -> None:
    import uvicorn

    uvicorn.run("app.main:app", host=settings.host, port=settings.port, reload=False)


if __name__ == "__main__":
    main()
