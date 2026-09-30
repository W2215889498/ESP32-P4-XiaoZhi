"""End-to-end smoke test: pretend to be the device over the Xiaozhi protocol.

Usage:  .venv\\Scripts\\python.exe tools\\ws_smoke_test.py [ws://host:port/xiaozhi/v1/]

Sends hello -> listen start -> 1s of Opus audio -> listen stop, then prints the
JSON replies and counts the binary (Opus TTS) frames it receives.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys

import numpy as np
import websockets

# reuse the server's codec (it also fixes up libopus discovery on Windows)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from app.audio import OpusCodec  # noqa: E402

WS_URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8000/xiaozhi/v1/"
SR = 16000
FRAME_MS = 60
SAMPLES = SR * FRAME_MS // 1000


async def main() -> None:
    codec = OpusCodec(SR, 1, FRAME_MS)
    async with websockets.connect(WS_URL, max_size=None) as ws:
        await ws.send(
            json.dumps(
                {
                    "type": "hello",
                    "version": 1,
                    "transport": "websocket",
                    "audio_params": {
                        "format": "opus",
                        "sample_rate": SR,
                        "channels": 1,
                        "frame_duration": FRAME_MS,
                    },
                }
            )
        )
        print("<-", await ws.recv())

        await ws.send(json.dumps({"type": "listen", "state": "start", "mode": "manual"}))

        t = np.arange(SAMPLES, dtype=np.float32)
        for i in range(16):  # 16 * 60ms ~= 1s of speech-like tone
            phase = 2 * np.pi * 220.0 * (i * SAMPLES + t) / SR
            pcm = (np.sin(phase) * 6000).astype("<i2").tobytes()
            for packet in codec.encode(pcm):
                await ws.send(packet)

        await ws.send(json.dumps({"type": "listen", "state": "stop"}))

        binary = 0
        types: list[str] = []
        while True:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=30)
            except asyncio.TimeoutError:
                print("!! timeout waiting for the server")
                break
            if isinstance(msg, bytes):
                binary += 1
                continue
            data = json.loads(msg)
            types.append(data.get("type", "?"))
            detail = data.get("state") or (data.get("text") or "")[:40]
            print("<-", data.get("type"), detail)
            if data.get("type") == "tts" and data.get("state") == "stop":
                break

        print("\nmessages :", types)
        print("opus frames received:", binary)
        ok = binary > 0 and "stt" in types and "tts" in types
        print("RESULT:", "PASS" if ok else "FAIL")


if __name__ == "__main__":
    asyncio.run(main())
