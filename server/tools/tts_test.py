"""一键测试当前 .env 配置的 TTS（文字转语音）。

用法（在 server 目录下）：
    .\\.venv\\Scripts\\python.exe tools\\tts_test.py
    .\\.venv\\Scripts\\python.exe tools\\tts_test.py "自定义测试文本"

会把合成结果保存为 tools/tts_test_out.wav（16k 单声道），直接双击试听即可。
"""

from __future__ import annotations

import asyncio
import sys
import wave
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVER_DIR))

from app.config import settings  # noqa: E402
from app.providers.tts import build_tts  # noqa: E402

TEXT = sys.argv[1] if len(sys.argv) > 1 else "你好，我是TK助手，这是一段语音合成测试。"


async def main() -> int:
    print(f"provider = {settings.tts_provider}")
    print(f"base_url = {settings.tts_base_url or '(edge 默认)'}")
    print(f"model    = {settings.tts_model}    voice = {settings.tts_voice}")

    tts = build_tts()
    print(f"-> 实际使用: {tts.name}")
    print(f"-> 合成文本: {TEXT}")

    try:
        pcm = await tts.synthesize(TEXT)
    except Exception as exc:  # noqa: BLE001
        print(f"!! 合成失败: {exc.__class__.__name__}: {exc}")
        return 1

    if not pcm:
        print("!! 合成结果为空（服务返回了空音频）")
        return 1

    out = SERVER_DIR / "tools" / "tts_test_out.wav"
    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(settings.tts_sample_rate)
        w.writeframes(pcm)

    seconds = len(pcm) / 2 / settings.tts_sample_rate
    print(f"OK: {len(pcm)} bytes, {seconds:.1f}s -> {out}")
    print("（双击该 wav 试听；再跑一次覆盖）")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
