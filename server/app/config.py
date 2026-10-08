"""Runtime configuration, loaded from environment / .env (prefix XZ_)."""

from __future__ import annotations

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=".env", env_file_encoding="utf-8", env_prefix="XZ_", extra="ignore"
    )

    # --- server ---
    host: str = "0.0.0.0"
    port: int = 8000
    # Advertised to the device in the config endpoint. Empty => derive from the
    # request Host header (works fine on a LAN). Set it when behind a proxy/NAT.
    public_ws_url: str = ""

    # --- LLM (DeepSeek, OpenAI compatible) ---
    llm_provider: str = "deepseek"  # deepseek | openai | mock
    llm_base_url: str = "https://api.deepseek.com"
    llm_api_key: str = ""
    llm_model: str = "deepseek-chat"
    llm_temperature: float = 0.7
    llm_max_tokens: int = 512
    llm_system_prompt: str = (
        "你叫TK助手，是一个运行在 ESP32 上的语音助手。"
        "回答要口语化、简洁，适合朗读，通常不超过三句话，不要使用 Markdown 或表情符号。"
        "始终使用简体中文回答，不要使用繁体字。"
        "当用户想控制开发板上的硬件（开关灯、继电器等）时，调用提供的工具去执行，"
        "并简短地告诉用户执行结果。"
        "重要：无论你是否记得引脚的当前状态，每次用户要求打开/关闭或设置高/低电平时，"
        "都必须实际调用工具执行，绝不能凭对话历史或猜测回答“已经是目标状态/无需操作”；"
        "一切以工具返回结果为准。"
    )

    # --- MCP（设备端工具，AI 控制硬件）---
    # 服务端作为 MCP 客户端：initialize -> tools/list -> 交给 DeepSeek 做 function
    # calling -> tools/call 下发到设备执行。
    mcp_enabled: bool = True
    mcp_request_timeout: float = 5.0
    mcp_init_retry: int = 4
    llm_tool_max_rounds: int = 4
    # 模型直接调用工具、一句都没说时，先播放一句垫场话（留空则关闭）
    llm_tool_filler: str = "好的，我来处理。"

    # --- ASR (speech to text) ---
    asr_provider: str = "local"  # local（faster-whisper，无需 Key）| openai | mock
    asr_base_url: str = "https://api.siliconflow.cn/v1"
    asr_api_key: str = ""
    asr_model: str = "FunAudioLLM/SenseVoiceSmall"
    asr_language: str = "zh"
    # 本地 faster-whisper（XZ_ASR_PROVIDER=local 时生效，无需 Key）
    asr_local_model: str = "small"  # tiny/base/small/medium/large-v3
    asr_device: str = "auto"  # auto | cpu | cuda

    # --- TTS (text to speech) ---
    tts_provider: str = "edge"  # edge (free, no key) | openai | mock
    tts_voice: str = "zh-CN-XiaoxiaoNeural"
    tts_base_url: str = ""
    tts_api_key: str = ""
    tts_model: str = "tts-1"
    tts_sample_rate: int = 16000

    # --- audio / protocol ---
    default_sample_rate: int = 16000
    default_frame_ms: int = 60
    max_utterance_sec: int = 30
    # simple energy VAD used in "auto" listening mode
    vad_threshold: int = 350
    vad_silence_ms: int = 800


settings = Settings()
