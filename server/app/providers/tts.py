"""TTS providers (text -> mono PCM16LE at settings.tts_sample_rate)."""

from __future__ import annotations

import logging
from abc import ABC, abstractmethod

import httpx
import numpy as np

from ..audio import decode_compressed_to_pcm16, float_to_pcm16
from ..config import settings

log = logging.getLogger("xz.tts")


class TTS(ABC):
    name = "base"

    @abstractmethod
    async def synthesize(self, text: str) -> bytes:
        """Return PCM16LE mono at ``settings.tts_sample_rate``."""


class EdgeTTS(TTS):
    """Microsoft Edge neural voices - free, no API key, good Chinese quality."""

    name = "edge"

    async def synthesize(self, text: str) -> bytes:
        import edge_tts

        comm = edge_tts.Communicate(text, settings.tts_voice)
        buf = bytearray()
        async for chunk in comm.stream():
            if chunk.get("type") == "audio" and chunk.get("data"):
                buf += chunk["data"]
        if not buf:
            log.warning("edge-tts returned no audio for %r", text[:40])
            return b""
        return decode_compressed_to_pcm16(bytes(buf), settings.tts_sample_rate)


class OpenAICompatTTS(TTS):
    """OpenAI-compatible ``/audio/speech`` (returns MP3/other)."""

    name = "openai"

    async def synthesize(self, text: str) -> bytes:
        url = settings.tts_base_url.rstrip("/") + "/audio/speech"
        headers = {"Authorization": f"Bearer {settings.tts_api_key}"}
        payload = {
            "model": settings.tts_model,
            "voice": settings.tts_voice,
            "input": text,
            "response_format": "mp3",
        }
        timeout = httpx.Timeout(connect=10.0, read=60.0, write=30.0, pool=10.0)
        async with httpx.AsyncClient(timeout=timeout) as client:
            resp = await client.post(url, headers=headers, json=payload)
            resp.raise_for_status()
            data = resp.content
        return decode_compressed_to_pcm16(data, settings.tts_sample_rate)


class FallbackTTS(TTS):
    """先试主引擎，失败后本会话内自动切换到备用引擎（例如 edge 被墙时切硅基流动）。"""

    name = "fallback"

    def __init__(self, primary: TTS, secondary: TTS):
        self._primary = primary
        self._secondary = secondary
        self._use_secondary = False

    async def synthesize(self, text: str) -> bytes:
        if not self._use_secondary:
            try:
                return await self._primary.synthesize(text)
            except Exception as exc:  # noqa: BLE001 - 网络类错误都要兜底
                log.warning(
                    "%s TTS 失败(%s)，本会话切换到 %s",
                    self._primary.name,
                    exc.__class__.__name__,
                    self._secondary.name,
                )
                self._use_secondary = True
        return await self._secondary.synthesize(text)


class MockTTS(TTS):
    """A short beep per sentence, so you can verify the audio path offline."""

    name = "mock"

    async def synthesize(self, text: str) -> bytes:
        rate = settings.tts_sample_rate
        dur = min(1.2, 0.25 + 0.05 * len(text))
        t = np.arange(int(rate * dur), dtype=np.float32) / rate
        wave = 0.25 * np.sin(2 * np.pi * 660 * t)
        fade = np.minimum(1.0, np.minimum(t / 0.02, (dur - t) / 0.02)).clip(0.0, 1.0)
        return float_to_pcm16(wave * fade)


def build_tts() -> TTS:
    provider = settings.tts_provider.lower()
    if provider == "edge":
        edge = EdgeTTS()
        if settings.tts_api_key:
            log.info(
                "TTS provider: edge-tts (%s)，备用: %s @ %s",
                settings.tts_voice,
                settings.tts_model,
                settings.tts_base_url or "(未配置 base_url)",
            )
            return FallbackTTS(edge, OpenAICompatTTS())
        log.info("TTS provider: edge-tts (%s)", settings.tts_voice)
        return edge
    if provider in ("openai", "siliconflow") and settings.tts_api_key:
        log.info("TTS provider: openai-compatible (%s / %s)", settings.tts_base_url, settings.tts_model)
        return OpenAICompatTTS()
    if provider != "mock":
        log.warning("TTS provider %r unavailable (missing XZ_TTS_API_KEY?); falling back to mock", provider)
    return MockTTS()
