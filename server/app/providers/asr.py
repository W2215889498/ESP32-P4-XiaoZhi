"""Automatic Speech Recognition providers (device audio -> text)."""

from __future__ import annotations

import asyncio
import logging
import os
import sys
from abc import ABC, abstractmethod
from pathlib import Path

import httpx
import numpy as np

from ..audio import to_wav_bytes
from ..config import is_local_service_url, settings

log = logging.getLogger("xz.asr")


class ASR(ABC):
    name = "base"

    @abstractmethod
    async def transcribe(self, pcm: bytes, sample_rate: int) -> str:
        ...


class OpenAICompatASR(ASR):
    """OpenAI-compatible ``/audio/transcriptions`` (OpenAI, SiliconFlow/SenseVoice, Groq, ...)."""

    name = "openai"

    async def transcribe(self, pcm: bytes, sample_rate: int) -> str:
        url = settings.asr_base_url.rstrip("/") + "/audio/transcriptions"
        wav = to_wav_bytes(pcm, sample_rate)
        files = {"file": ("audio.wav", wav, "audio/wav")}
        data = {"model": settings.asr_model, "response_format": "json"}
        if settings.asr_language:
            data["language"] = settings.asr_language
        headers = {}
        if settings.asr_api_key:
            headers["Authorization"] = f"Bearer {settings.asr_api_key}"
        timeout = httpx.Timeout(connect=10.0, read=60.0, write=60.0, pool=10.0)
        async with httpx.AsyncClient(timeout=timeout) as client:
            resp = await client.post(url, headers=headers, data=data, files=files)
            resp.raise_for_status()
            payload = resp.json()
        text = (payload.get("text") or "").strip()
        log.info("ASR(%s) -> %r", settings.asr_model, text[:80])
        return text


class LocalWhisperASR(ASR):
    """本地 faster-whisper：不需要任何 Key，识别在本机完成。

    - 模型首次运行自动下载（国内默认走 hf-mirror.com 镜像）；
    - `.env` 里 `XZ_ASR_LOCAL_MODEL` 选 tiny/base/small/medium/large-v3（默认 small）；
    - `XZ_ASR_DEVICE=auto|cpu|cuda`，auto 会先试 GPU（需 CUDA/cuDNN），失败退 CPU int8。
    """

    name = "local"
    _model = None  # 进程内共享，避免每条语音重复加载

    def _load_model(self, device: str):
        os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
        os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")
        # hf-mirror 不支持 Xet 存储协议，强制走普通下载
        os.environ.setdefault("HF_HUB_DISABLE_XET", "1")
        # 若通过 pip 装了 nvidia-*-cu12 运行库，把 DLL 目录加入搜索路径（Windows）
        try:
            nvidia_dir = Path(sys.prefix, "Lib", "site-packages", "nvidia")
            for pkg in nvidia_dir.glob("*"):
                bin_dir = pkg / "bin"
                if bin_dir.is_dir():
                    os.add_dll_directory(str(bin_dir))  # type: ignore[attr-defined]
        except Exception:  # noqa: BLE001 - 非 Windows/目录不存在都无所谓
            pass

        from faster_whisper import WhisperModel

        if device == "cuda":
            try:
                return WhisperModel(settings.asr_local_model, device="cuda", compute_type="float16")
            except Exception as exc:  # noqa: BLE001
                log.warning("本地 ASR 无法使用 GPU(%s)，改用 CPU int8", exc.__class__.__name__)
                device = "cpu"
        return WhisperModel(settings.asr_local_model, device="cpu", compute_type="int8")

    def _ensure_model(self):
        if LocalWhisperASR._model is None:
            device = settings.asr_device.lower()
            if device == "auto":
                # 先试 GPU，失败（没装 CUDA/cuDNN 或没有显卡）会自动退回 CPU int8
                device = "cuda"
            log.info(
                "加载本地 ASR 模型 %s（device=%s，首次运行会下载模型，请稍候）…",
                settings.asr_local_model,
                device,
            )
            LocalWhisperASR._model = self._load_model(device)
            log.info("本地 ASR 模型就绪")
        return LocalWhisperASR._model

    def _transcribe_sync(self, pcm: bytes, sample_rate: int) -> str:
        model = self._ensure_model()
        audio = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        if sample_rate != 16000 and len(audio) > 1:
            n = max(1, int(len(audio) * 16000 / sample_rate))
            audio = np.interp(
                np.linspace(0.0, len(audio) - 1.0, n), np.arange(len(audio)), audio
            ).astype(np.float32)
        segments, _info = model.transcribe(
            audio,
            language=settings.asr_language or None,
            beam_size=1,
            vad_filter=True,
            condition_on_previous_text=False,
            # 引导 Whisper 输出简体中文（否则它经常转写为繁体）
            initial_prompt="以下是普通话的句子，请使用简体中文转写。",
        )
        text = "".join(seg.text for seg in segments).strip()
        log.info("ASR(local/%s) -> %r", settings.asr_local_model, text[:80])
        return text

    async def transcribe(self, pcm: bytes, sample_rate: int) -> str:
        return await asyncio.to_thread(self._transcribe_sync, pcm, sample_rate)


class MockASR(ASR):
    """Offline stand-in so the LLM/TTS path can be tested without an ASR key."""

    name = "mock"

    _PHRASES = [
        "你好小智，今天天气怎么样？",
        "帮我讲一个关于人工智能的笑话。",
        "现在几点了？",
    ]
    _i = 0

    async def transcribe(self, pcm: bytes, sample_rate: int) -> str:
        text = self._PHRASES[self._i % len(self._PHRASES)]
        type(self)._i += 1
        log.warning("ASR not configured -> mock result: %r", text)
        return text


def _local_available() -> bool:
    try:
        import faster_whisper  # noqa: F401
        return True
    except ImportError:
        return False


def build_asr() -> ASR:
    provider = settings.asr_provider.lower()
    if provider in ("local", "whisper", "faster-whisper"):
        log.info("ASR provider: 本地 faster-whisper (%s)", settings.asr_local_model)
        return LocalWhisperASR()
    if provider == "openai" and settings.asr_api_key:
        log.info("ASR provider: openai-compatible (%s)", settings.asr_base_url)
        return OpenAICompatASR()
    if provider == "openai" and is_local_service_url(settings.asr_base_url):
        log.info("ASR provider: openai-compatible 本地服务 (%s, 免 Key)", settings.asr_base_url)
        return OpenAICompatASR()
    if provider != "mock" and _local_available():
        log.warning("ASR provider %r 需要 Key，本次自动改用本地 faster-whisper", provider)
        return LocalWhisperASR()
    if provider != "mock":
        log.warning("ASR provider %r needs XZ_ASR_API_KEY; falling back to mock", provider)
    return MockASR()
