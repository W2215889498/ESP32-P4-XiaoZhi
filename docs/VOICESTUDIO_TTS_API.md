# VoiceStudio TTS 服务接口文档（对接 TK 助手 · 文字转语音）

> **用途**：同事从 [VoiceStudio](https://github.com/debpalash/VoiceStudio) 提取的"纯净版服务"，作为本项目（TK 助手）的**文字转语音（TTS）**引擎。按本文档实现/确认接口后，本项目**无需改代码**，只改几行 `.env` 即可接入。
>
> 对接拓扑：
> ```
> 大模型逐句输出 ──> 本项目 server ──(HTTP, 本文档接口)──> 同事的 VoiceStudio TTS 服务
>                                          │
>                            返回二进制音频 ◄─┘ ──> 本地解码/重采样 ──> Opus ──> 开发板喇叭
> ```

---

## 1. 总体要求（TL;DR）

| 项目 | 要求 |
|---|---|
| 监听地址 | `http://127.0.0.1:3900`（VoiceStudio 原版默认端口；换端口请交付时注明） |
| 必须实现的接口 | `POST /v1/audio/speech`（**OpenAI 兼容**语音合成） |
| 请求体 | JSON：`{"model": ..., "voice": ..., "input": 文本, "response_format": "mp3"}` |
| 返回体 | **二进制音频字节流**（不是 JSON、不是 base64） |
| 推荐返回格式 | **WAV**（16-bit PCM 单声道，任何采样率均可）；MP3 / FLAC 也可（我方自动识别容器并重采样到 16k） |
| 鉴权 | 本机回环不要求；若实现 Bearer，我方可选携带（见 §2.3） |
| 单次文本 | 一句中文（一般 ≤ 50 字）；尽量 **2~3 秒内**返回 |

---

## 2. 接口契约：`POST /v1/audio/speech`

### 2.1 请求

- Method：`POST`
- Header：`Content-Type: application/json`（若配置了 Key，我方会带 `Authorization: Bearer <key>`）
- Body（JSON）：

| 字段 | 我方发送的值 | 对方实现要求 |
|---|---|---|
| `input` | 待合成文本（中文，一句） | **必须支持**；UTF-8 |
| `voice` | 配置的音色名（见 §4.1） | 建议支持；**收到未知/空音色时用默认音色，不要报错** |
| `model` | 配置的模型名（如 `omnivoice`） | **接受并忽略任意字符串**；若有多模型可按值路由，未知时用默认 |
| `response_format` | 固定发送 `"mp3"`（历史兼容） | **建议直接忽略此字段**，返回你们最优格式（WAV 最佳）；若按值实现，需要支持 `mp3` 与 `wav` |

### 2.2 响应

- **成功 200**：Body 为**音频二进制**（`Content-Type: audio/wav` 或 `audio/mpeg` 等均可）。
  - 推荐 **WAV / 16-bit PCM**：我方用 miniaudio 解码，**WAV、MP3、FLAC 都会自动识别**，任意采样率/声道数也会自动转成 16k 单声道。
  - **不要**把音频塞进 JSON（如 base64）——原样返回字节流最稳。
- **失败**：返回非 200 + JSON 错误体（FastAPI 默认 `{"detail": "..."}` 即可），我方记录日志并按配置降级。

### 2.3 鉴权（可选）

- 本机回环（127.0.0.1）场景不要求鉴权。
- 我方行为：`XZ_TTS_API_KEY` 非空时带 `Authorization: Bearer <key>`；为空则**不带任何认证头**。服务端两种都需兼容。

---

## 3. 音色与声音克隆（按你们实现情况二选一）

### 3.1 固定音色集（最简）
服务端内置若干音色，如 `default` / `female1` / `male1`，我方通过 `XZ_TTS_VOICE=<音色名>` 透传。请交付时附上**音色名清单**。

### 3.2 声音克隆
推荐把"参考音频 → 音色"的注册/管理放在**服务端一侧**（例如先注册得到 `voice_id`），我方只透传 `voice` 字符串，例如：
- 注册接口（由你们定义）：上传参考音频 → 返回 `voice_id`
- 合成时：`{"voice": "clone_xxx", "input": "..."}`

> 我方可直接使用任意 voice 字符串，无需改代码；但只要"未知 voice 不报错、退回默认"即可保证协议稳定。

---

## 4. 本项目如何接入（我方操作）

### 4.1 修改 `server/.env`

```dotenv
XZ_TTS_PROVIDER=openai
XZ_TTS_BASE_URL=http://127.0.0.1:3900/v1
XZ_TTS_API_KEY=              # 本地服务留空即可（已支持免 Key）；如需鉴权填你们的 key
XZ_TTS_MODEL=omnivoice       # 任意值，服务端忽略/路由均可
XZ_TTS_VOICE=default         # 换成你们提供的音色名
XZ_TTS_SAMPLE_RATE=16000     # 保持默认
```

重启服务端（`cd server` + `.\run.ps1`）。日志应显示：
`TTS provider: openai-compatible 本地服务 (http://127.0.0.1:3900/v1 / omnivoice, 免 Key)`

### 4.2 我方调用流程（供双方对齐）

1. LLM 每输出完一句 → 调 `POST {XZ_TTS_BASE_URL}/audio/speech`
2. Body：`{"model", "voice", "input", "response_format": "mp3"}`
3. 拿响应字节 → miniaudio 解码 → 重采样为 **16k 单声道 PCM** → Opus(60ms/帧) → WebSocket 下发开发板
4. 一段回复会分成多句多次请求（逐句合成、边合成边播，降低首句延迟）

### 4.3 一键测试脚本（推荐先跑这个）

```powershell
cd C:\Users\22158\Documents\esp32_projects\ESP32-P4-XiaoZhi\server
.\.venv\Scripts\python.exe tools\tts_test.py
```

- 会用当前 `.env` 配置合成一句「你好，我是TK助手，这是一段语音合成测试。」并保存为 `server/tools/tts_test_out.wav`，双击即可试听
- 可自定义文本：`.\.venv\Scripts\python.exe tools\tts_test.py "任意文本"`
- 想临时切回旧引擎对比：设置环境变量 `XZ_TTS_PROVIDER=edge` 再运行

### 4.4 出问题时的降级 / 其它部署方式

- **随时回退**：`XZ_TTS_PROVIDER=edge`（免费微软语音）或 `mock`（蜂鸣音自测）；改回 `openai` 即恢复接你们的服务
- **服务在另一台机器**：`XZ_TTS_BASE_URL=http://<对方IP>:3900/v1`，对方放行防火墙
- 如果想要"同事服务失败 → 自动回退 edge"：告诉我，我在 `tts.py` 加 5 行反向兜底（目前默认是 edge 失败时兜底到 openai，方向相反）

---

## 5. 边界与约定

- **并发**：单设备、低负载；同一时刻最多 1~2 个合成请求
- **合法性**：只通过 HTTP 接口对接，**不要把 VoiceStudio 源码并入本项目仓库**（AGPL-3.0 传染性）
- **内容安全**：合成的文本来自 LLM 回复（日常对话），服务端可自行过滤
- **日志**：我方会记录每句文本与耗时，便于双方联调

---

## 附：如果同事的接口不是 OpenAI 兼容格式

请尽量向上面的 OpenAI 兼容格式靠拢（改动最小、事实标准）。
若无法兼容，请把实际接口（路径、请求字段、响应结构、音频格式）发回来，我在 `server/app/providers/tts.py` 新增一个 provider（约 30 行）适配，其余逻辑不动。
