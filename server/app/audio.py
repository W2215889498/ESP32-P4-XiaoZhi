"""Audio helpers: Opus codec + PCM utilities.

The device speaks Opus (16 kHz / mono / 60 ms frames by default). ``opuslib``
is used as it wraps the system libopus via ctypes and needs no build step.
If libopus is missing the server still runs, but Opus (and therefore the
device audio path) is unavailable - ``opus_available()`` reports that.
"""

from __future__ import annotations

import logging
import os
import sys

import numpy as np

log = logging.getLogger("xz.audio")

SAMPLE_WIDTH = 2  # int16 mono frames
_OPUSLIB = None
_OPUS_ERR: str | None = None


def _make_opus_discoverable() -> None:
    """Help ``opuslib`` locate libopus.

    On Windows the ``pyogg`` wheel ships ``opus.dll`` but ``ctypes.util.find_library``
    only searches ``PATH``, so expose pyogg's folder (or ``XZ_OPUS_DIR``) first.
    """
    candidates: list[str] = []
    env_dir = os.environ.get("XZ_OPUS_DIR")
    if env_dir:
        candidates.append(env_dir)
    if sys.platform == "win32":
        try:
            import pyogg

            candidates.append(os.path.dirname(os.path.abspath(pyogg.__file__)))
        except Exception:
            pass
    for folder in candidates:
        if not os.path.isfile(os.path.join(folder, "opus.dll")):
            continue
        path = os.environ.get("PATH", "")
        if folder not in path.split(os.pathsep):
            os.environ["PATH"] = folder + os.pathsep + path
        try:
            os.add_dll_directory(folder)  # py3.8+, resolves opus.dll's own deps
        except Exception:
            pass
        log.debug("exposed libopus from %s", folder)
        return


def opus_available() -> bool:
    _load_opus()
    return _OPUSLIB is not None


def opus_error() -> str | None:
    _load_opus()
    return _OPUS_ERR


def _load_opus() -> None:
    global _OPUSLIB, _OPUS_ERR
    if _OPUSLIB is not None or _OPUS_ERR is not None:
        return
    _make_opus_discoverable()
    try:
        import opuslib

        # touch the API so a missing shared library fails here, not per-frame
        opuslib.Encoder(16000, 1, "audio")
        _OPUSLIB = opuslib
        log.info("libopus loaded (opuslib)")
    except Exception as exc:  # pragma: no cover - depends on host
        _OPUS_ERR = str(exc)
        log.warning("libopus unavailable, Opus audio disabled: %s", exc)


class OpusCodec:
    """Frame-based Opus <-> PCM16LE converter for one audio stream."""

    def __init__(self, sample_rate: int = 16000, channels: int = 1, frame_ms: int = 60):
        _load_opus()
        if _OPUSLIB is None:
            raise RuntimeError(f"libopus not available: {_OPUS_ERR}")
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_ms = frame_ms
        self.frame_samples = max(1, sample_rate * frame_ms // 1000)
        self._max_samples = sample_rate * 120 // 1000  # opus max frame = 120 ms
        self._enc = _OPUSLIB.Encoder(sample_rate, channels, "audio")
        self._dec = _OPUSLIB.Decoder(sample_rate, channels)

    @property
    def frame_bytes(self) -> int:
        return self.frame_samples * SAMPLE_WIDTH * self.channels

    def decode(self, packet: bytes) -> bytes:
        """One Opus packet -> PCM16LE (mono)."""
        return self._dec.decode(packet, self._max_samples)

    def decode_many(self, packets: list[bytes]) -> bytes:
        return b"".join(self.decode(p) for p in packets if p)

    def encode(self, pcm: bytes) -> list[bytes]:
        """PCM16LE -> list of Opus packets, zero-padding the last partial frame."""
        step = self.frame_bytes
        out: list[bytes] = []
        for i in range(0, len(pcm), step):
            chunk = pcm[i : i + step]
            if len(chunk) < step:
                chunk = chunk + b"\x00" * (step - len(chunk))
            out.append(self._enc.encode(chunk, self.frame_samples))
        return out


# --------------------------------------------------------------------------- #
# PCM utilities
# --------------------------------------------------------------------------- #
def pcm_rms(pcm: bytes) -> float:
    """Root-mean-square level of a PCM16LE buffer (0..32767 scale)."""
    if len(pcm) < 2:
        return 0.0
    a = np.frombuffer(pcm, dtype="<i2").astype(np.float32)
    if a.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(a * a)))


def float_to_pcm16(samples: np.ndarray) -> bytes:
    """float32 in [-1, 1] -> PCM16LE bytes."""
    clipped = np.clip(samples, -1.0, 1.0)
    return (clipped * 32767.0).astype("<i2").tobytes()


def pcm16_to_float(pcm: bytes) -> np.ndarray:
    return np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0


def resample_linear(samples: np.ndarray, src_rate: int, dst_rate: int) -> np.ndarray:
    """Cheap linear resampler (fine for 8k/16k/24k speech)."""
    if src_rate == dst_rate or samples.size == 0:
        return samples
    n_out = int(round(samples.size * dst_rate / src_rate))
    if n_out <= 0:
        return np.zeros(0, dtype=np.float32)
    x_old = np.linspace(0.0, 1.0, samples.size, endpoint=False)
    x_new = np.linspace(0.0, 1.0, n_out, endpoint=False)
    return np.interp(x_new, x_old, samples).astype(np.float32)


def to_wav_bytes(pcm: bytes, sample_rate: int, channels: int = 1) -> bytes:
    """Wrap raw PCM16LE in a WAV container (for ASR upload)."""
    import io
    import wave

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)
    return buf.getvalue()


def decode_compressed_to_pcm16(data: bytes, sample_rate: int) -> bytes:
    """Decode MP3/FLAC/WAV (whatever miniaudio supports) to mono PCM16LE."""
    import miniaudio

    decoded = miniaudio.decode(
        data,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=sample_rate,
    )
    return bytes(decoded.samples)
