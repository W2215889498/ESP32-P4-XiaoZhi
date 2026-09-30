"""Provider factories: ASR / LLM / TTS.

Each provider degrades gracefully: if it needs an API key that is not
configured it falls back to a mock so the whole pipeline still runs end to
end while you are wiring things up.
"""

from __future__ import annotations

import logging

from .asr import ASR, build_asr
from .llm import LLM, build_llm
from .tts import TTS, build_tts

log = logging.getLogger("xz.providers")

__all__ = ["ASR", "LLM", "TTS", "build_asr", "build_llm", "build_tts"]
