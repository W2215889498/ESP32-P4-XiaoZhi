# 小智服务端（方案 A）· FastAPI

一个自托管的「小智协议」服务端：设备端只要说话，它会做
**ASR（语音转文字）→ LLM（DeepSeek）→ TTS（文字转语音）**，把音频流回设备。

```
ESP32-P4 (esp_xiaozhi)  ──WS/Opus──▶  本服务端
                                        ├─ ASR  : SenseVoice / Whisper / OpenAI 兼容
                                        ├─ LLM  : DeepSeek（你的 API Key）
                                        └─ TTS  : edge-tts（免费）/ OpenAI 兼容
```

- 设备端**只需要说小智协议**，完全不用知道 DeepSeek；Key 只存在服务器里。
- 没有配置 Key 的环节会自动降级为 `mock`，所以**开箱即可跑通整条链路**自检。

---

## 1. 快速开始

```powershell
cd server
.\run.ps1
```

`run.ps1` 会自动：建虚拟环境 → 装依赖 → 生成 `.env`（首次）→ 启动服务。

然后编辑 `server\.env`，填上你的 DeepSeek Key：

```ini
XZ_LLM_API_KEY=sk-你的Key
```

访问 <http://127.0.0.1:8000/> 可看到状态页；`/docs` 是接口文档。

> 局域网内给设备用时，防火墙要放行 8000 端口，并用**本机局域网 IP**（如 `192.168.1.10`）而不是 `127.0.0.1`。

---

## 2. 目录结构

```
server/
  run.ps1                 一键启动（建 venv / 装依赖 / 生成 .env / 跑 uvicorn）
  requirements.txt
  .env.example            配置模板
  app/
    main.py               FastAPI：/health、/xiaozhi/ota/、WS /xiaozhi/v1/
    config.py             配置（前缀 XZ_，读 .env）
    protocol.py           小智协议消息构造
    audio.py              Opus 编解码 + PCM/重采样/WAV 工具
    session.py            单个连接的协议状态机 + 对话流水线
    providers/            asr.py / llm.py / tts.py（可插拔）
  tools/
    ws_smoke_test.py      模拟设备做端到端自检
```

---

## 3. 接口

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | 状态页 |
| GET | `/health` | 健康检查（含 opus/各 provider 状态）|
| GET/POST | `/xiaozhi/ota/` | **设备发现/配置**：返回要连的 WebSocket 地址 |
| WS | `/xiaozhi/v1/` | 小智双向流式协议 |

配置端点返回示例：

```json
{
  "server_time": { "timestamp": 1790586300222, "timezone_offset": 480 },
  "firmware": { "version": "0.0.0", "url": "" },
  "websocket": { "url": "ws://192.168.1.10:8000/xiaozhi/v1/", "token": "" }
}
```

---

## 4. 协议消息速查

**设备 → 服务器**

| 消息 | 说明 |
|---|---|
| `hello` (JSON) | 建会话，含 `audio_params`：`{format:"opus", sample_rate:16000, channels:1, frame_duration:60}` |
| 二进制帧 | 一帧 Opus 麦克风音频 |
| `listen` | `{"state":"start"\|"stop"\|"detect", "mode":"auto"\|"manual"}` |
| `abort` | 打断当前播放 |
| `mcp` | JSON-RPC（AI 控制设备）|

**服务器 → 设备**

| 消息 | 说明 |
|---|---|
| `hello` (JSON) | 回 `session_id` + `audio_params` |
| `stt` | 识别出的文字 |
| `tts` | `start` / `sentence_start`(带句子文本) / `stop` |
| 二进制帧 | TTS 的 Opus 音频 |
| `llm` | 情绪（可做表情）|
| `mcp` | MCP 响应 |

**聆听模式**：`manual`（按住说话）/ `auto`（本地能量 VAD 自动断句，默认）/ `realtime`。
`session.py` 里 `vad_threshold`、`vad_silence_ms` 可在 `.env` 调。

**MCP（AI 控制设备硬件）**：设备端是小智协议中的 MCP Server（工具跑在开发板上，
如 `self.gpio.set_output`），本服务端是 MCP Client：

```
hello 完成 ──▶ 服务端 initialize ──▶ tools/list 拿工具 schema
用户说话 ──▶ ASR ──▶ DeepSeek(function calling)
              ├─ 文本增量 ──▶ 逐句 TTS 下发
              └─ tool_calls ──▶ 服务端 tools/call 下发到设备执行 ──▶ 结果回填 ──▶ 继续生成
```

可用 `XZ_MCP_ENABLED=false` 关闭；`XZ_MCP_INIT_RETRY` / `XZ_MCP_REQUEST_TIMEOUT`
控制初始化重试与请求超时（设备 hello 后才会创建 MCP，存在毫秒级竞态，故有重试）。

---

## 5. 把设备指到本服务端

设备启动时先请求一个「配置端点」拿 WebSocket 地址，所以两种做法：

1. **改配置端点地址**：把固件里默认的配置 URL（官方默认指向它的云服务）改成
   `http://<你的IP>:8000/xiaozhi/ota/`。设备就会拿到你返回的 WS 地址。
2. **直接写死 WS 地址**：在设备端配置里直接填 `ws://<你的IP>:8000/xiaozhi/v1/`，
   跳过配置端点。

> 下一步设备端接入：在当前 ESP-IDF 工程里加入 `espressif/esp_xiaozhi`，
> 音频 I/O 用 Waveshare BSP（ES7210 录音 / ES8311 播放），UI 复用现有 LVGL 界面。

---

## 6. 自检（不用真设备）

```powershell
server\.venv\Scripts\python.exe server\tools\ws_smoke_test.py ws://127.0.0.1:8000/xiaozhi/v1/
```

它会模拟设备：`hello → listen start → 送 1 秒 Opus 音频 → listen stop`，
然后打印收到的 JSON 消息并统计回来的 Opus 音频帧数。看到
`RESULT: PASS` 就说明整条链路（含真实 TTS）通了。

实测样例（未配任何 Key，ASR/LLM 走 mock，TTS 走真实 edge-tts）：

```
<- hello   (session_id, audio_params)
<- stt     你好小智，今天天气怎么样？
<- tts start
<- tts sentence_start
<- tts sentence_start
<- tts stop
opus frames received: 165
RESULT: PASS
```

---

## 7. 供应商配置

| 环节 | 默认 | 说明 |
|---|---|---|
| LLM | `deepseek` / `https://api.deepseek.com` / `deepseek-chat` | 填 `XZ_LLM_API_KEY` 即启用 |
| ASR | `local`（本机 faster-whisper） | **免费、无需 Key**、离线识别；也可换硅基流动 SenseVoice / OpenAI / Groq（填 `XZ_ASR_PROVIDER=openai` + Key） |
| TTS | `edge`（微软 Edge 语音） | **免费、无需 Key**，中文效果好；访问不了微软服务时换硅基流动（见下） |

**本地 ASR（默认，零配置）**：服务端直接在这台电脑上用 faster-whisper 识别语音，
不需要任何 API Key。首次识别会自动下载模型（国内自动走 `hf-mirror.com` 镜像），
默认 `small` 约 460MB；i9-14900K 实测约 2 倍实时（3.7s 语音 ≈ 1.7s 出字）。

- 想更准：`XZ_ASR_LOCAL_MODEL=medium`（约 1.5GB）或 `large-v3`（约 3GB）
- 想用 GPU：`pip install nvidia-cublas-cu12 nvidia-cudnn-cu12`，然后保持
  `XZ_ASR_DEVICE=auto`（会先试 CUDA，失败自动退回 CPU int8）
- 纯 CPU 机器建议 `XZ_ASR_LOCAL_MODEL=base`（约 150MB，更省算力）

想换供应商，只改 `.env` 里的 `XZ_*_PROVIDER` / `XZ_*_BASE_URL` / `XZ_*_MODEL`，
只要对方是 OpenAI 兼容接口即可，代码不用动。

国内网络连不上 edge-tts 时，推荐用硅基流动 TTS（Key 与 ASR 复用）：

```dotenv
XZ_TTS_PROVIDER=openai
XZ_TTS_BASE_URL=https://api.siliconflow.cn/v1
XZ_TTS_API_KEY=<你的硅基流动 Key>
XZ_TTS_MODEL=FunAudioLLM/CosyVoice2-0.5B
XZ_TTS_VOICE=FunAudioLLM/CosyVoice2-0.5B:alex
```

也可以保持 `XZ_TTS_PROVIDER=edge` 只填 `XZ_TTS_API_KEY`：edge 失败时会在本会话内
自动切换到上面的 OpenAI 兼容 TTS（避免每次回复都白等一次超时）。

---

## 8. 常见问题

- **`libopus MISSING`**：设备音频依赖 libopus。Windows 上本项目已通过 `pyogg`
  自带的 `opus.dll` 自动解决（`app/audio.py` 会把它的目录加进 DLL 搜索路径）。
  若仍失败，设 `XZ_OPUS_DIR=<含 opus.dll 的目录>`。Linux：`apt install libopus0`。
- **没有声音 / 全是 beep**：`XZ_TTS_PROVIDER=mock` 时会用蜂鸣音代替语音。
- **edge-tts 没反应**：它需要访问微软服务；公司网络被墙时换成 `XZ_TTS_PROVIDER=openai`
  指向硅基流动 CosyVoice（见 §7），或只填 `XZ_TTS_API_KEY` 让它自动兜底。
- **ASR 识别不准**：换更大的本地模型（`XZ_ASR_LOCAL_MODEL=medium/large-v3`）；
  也可以调开发板麦克风增益（menuconfig → XiaoZhi Assistant App → Microphone gain）。
- **延迟大**：本实现是「说完再回答」。要更低延迟可开 `realtime` 模式并做
  流式 ASR（边说边识别）；LLM 侧已经是流式逐句 TTS。
- **回声（自己跟自己说话）**：喇叭外放时麦克风会听到 TTS。产品必须做 AEC
  （ESP-SR 的 AFE 或板级方案），否则会自我打断。
- **安全**：不要把 API Key 放到固件里——只放这台服务器。

---

## 9. 已实现 / 待办

**已实现**：配置端点、WebSocket 协议（hello / listen / abort / mcp）、
Opus 编解码、能量 VAD 自动断句、ASR/LLM/TTS 可插拔 + 自动降级、流式逐句 TTS、
会话历史、打断；**MCP 客户端（initialize / tools/list / tools/call）+ DeepSeek
function calling**，AI 可以通过设备端工具控制硬件（固件示例：`self.gpio.set_output`）。

**待办**：流式 ASR（边说边出字）、MQTT+UDP 传输、TTS 边合成边推流（降低首包延迟）、
激活码/鉴权、并发多设备压力测试、更多设备端 MCP 工具（屏幕/音量/传感器等）。
