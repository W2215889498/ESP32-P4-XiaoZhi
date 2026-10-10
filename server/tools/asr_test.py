"""一键测试当前 .env 配置的 ASR（语音转文字）。

用法（在 server 目录下）：
    .\\.venv\\Scripts\\python.exe tools\\asr_test.py
    .\\.venv\\Scripts\\python.exe tools\\asr_test.py 路径\\到\\音频.wav

默认使用 tools/asr_test_zh.wav（内容：「你好小智，今天天气怎么样？」）。
"""

from __future__ import annotations

import asyncio
import sys
import wave
from pathlib import Path

import numpy as np

SERVER_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVER_DIR))

from app.config import settings  # noqa: E402
from app.providers.asr import build_asr  # noqa: E402

WAV = Path(sys.argv[1]) if len(sys.argv) > 1 else SERVER_DIR / "tools" / "asr_test_zh.wav"


async def main() -> int:
    print(f"provider = {settings.asr_provider}")
    print(f"base_url = {settings.asr_base_url or '(本机)'}")

    with wave.open(str(WAV), "rb") as w:
        sr, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    assert sw == 2, f"仅支持 16-bit wav，当前 {sw * 8}-bit"
    x = np.frombuffer(raw, dtype="<i2")
    if ch == 2:
        x = x.reshape(-1, 2).mean(axis=1).astype("<i2")
    print(f"audio    = {WAV.name}  {sr}Hz {ch}ch {len(x) / sr:.1f}s")

    asr = build_asr()
    print(f"-> 实际使用: {asr.name}")
    try:
        text = await asr.transcribe(x.tobytes(), sr)
    except Exception as exc:  # noqa: BLE001
        print(f"!! 识别失败: {exc.__class__.__name__}: {exc}")
        return 1
    print(f"识别结果: {text}")
    print("（期望：你好小智，今天天气怎么样？）")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
