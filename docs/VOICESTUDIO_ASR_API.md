# VoiceStudio ASR 服务接口文档（对接 TK 助手）

> **实际部署与已实现的适配**：同事实际部署的是「Whisper 语音转录 API」（FastAPI，端口 7778），接口为
> `POST /transcribe`（multipart：`file` + `language`，返回 `{"success":true,"result":{"text":...}}`），
> **不是** OpenAI 兼容格式。本项目已内置 provider **`voicestudio`** 完成适配，`.env` 配置：
>
> ```dotenv
> XZ_ASR_PROVIDER=voicestudio
> XZ_ASR_BASE_URL=http://192.168.5.102:7778
> ```
>
> 验证：`cd server` + `.\.venv\Scripts\python.exe tools\asr_test.py`
> （默认用 `server/tools/asr_test_zh.wav`，应输出「你好小智，今天天气怎么样？」）。
>
> **已知排障**：若服务返回 `{"success":false,"error":"[WinError 2] 系统找不到指定的文件。"}`，
> 说明服务内部缺文件——最常见是 **ffmpeg 未安装或不在服务进程的 PATH 中**（whisper 解码音频依赖
> ffmpeg，即使输入 WAV 也会调用它）。请在运行该服务的环境执行 `ffmpeg -version` 验证；安装后重启服务。
> 次常见原因：临时目录/模型文件路径在部署环境不存在（看服务端控制台 traceback 中 WinError 2 对应的文件名即可定位）。
>
> 下文保留的 OpenAI 兼容契约作为**备选方案**（对方改版或另有部署时使用）。

> **用途**：同事从 [VoiceStudio](https://github.com/debpalash/VoiceStudio) 提取的"纯净版 ASR 服务"，按本文档确认/实现接口后，本项目（TK 助手的 FastAPI 服务端 `server/`）**无需改代码**，只改 3 行 `.env` 即可接入。
>
> 对接拓扑：
> ```
> 开发板 ──(小智协议/WSS)──> 本项目 server ──(HTTP, 本文档接口)──> 同事的 VoiceStudio ASR 服务
> ```

---

## 1. 总体要求（TL;DR）

| 项目 | 要求 |
|---|---|
| 监听地址 | `http://127.0.0.1:3900`（VoiceStudio 原版默认端口；换端口请在交付时注明） |
| 必须实现的接口 | `POST /v1/audio/transcriptions`（**OpenAI 兼容**语音转写） |
| 上传音频格式 | **WAV：16000Hz、单声道、16-bit PCM**（我方保证格式） |
| 响应格式 | `{"text": "识别文本"}` |
| 语言 | 以中文为主；请求会带 `language=zh`（可忽略） |
| 单次音频时长 | ≤ 30 秒（我方一次对话上传一段；请保证 30 秒内返回） |
| 鉴权 | 本机回环（127.0.0.1）不要求；如支持 `Authorization: Bearer <key>` 更好 |

---

## 2. 接口契约：`POST /v1/audio/transcriptions`

### 2.1 请求

- Method：`POST`
- Content-Type：`multipart/form-data`
- 表单字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `file` | 文件 | WAV 音频，文件名 `audio.wav`，Content-Type `audio/wav`；**16kHz / 单声道 / 16-bit PCM** |
| `model` | 字符串 | 我方默认传 `FunAudioLLM/SenseVoiceSmall`；**请接受并忽略任意字符串**（不要因未知模型名报错） |
| `language` | 字符串 | 默认 `zh`；可忽略或用于提示识别语言 |
| `response_format` | 字符串 | 固定 `json` |

### 2.2 响应

- **成功 200**：

```json
{"text": "你好小智，今天天气怎么样？"}
```

- `text` 必须是最终识别文本（UTF-8 中文）。
- 额外字段（如 `duration`、`language`）可以带，我方只取 `text`。
- **失败**：返回 4xx/5xx + JSON 错误体即可（`{"error": {...}}` 或 FastAPI 默认 `{"detail": "..."}` 都兼容），我方会记录日志并让本轮对话降级。
- **超时**：我方客户端读超时 60 秒；请保证 30 秒内返回（正常 3~10 秒音频应在几秒内出结果）。

### 2.3 测试音频与期望结果

- 测试文件：**`server/tools/asr_test_zh.wav`**（随本项目仓库提供，约 3.6 秒）
- 内容：普通话「**你好小智，今天天气怎么样？**」
- 期望：`text` 字段包含 ≈ `你好小智，今天天气怎么样`（标点可有可无）

### 2.4 自测命令（Windows 自带 curl.exe）

```powershell
curl.exe -X POST http://127.0.0.1:3900/v1/audio/transcriptions `
  -F "file=@server\tools\asr_test_zh.wav;type=audio/wav" `
  -F "model=x" -F "language=zh" -F "response_format=json"
```

期望输出形如：`{"text":"你好小智,今天天气怎么样?"}`

---

## 3. 可选接口（二期，先不做不影响对接）

### 3.1 流式转写（"边说边出字"，保留原版实现的话请勿删除）

- `WS /v1/audio/transcriptions/stream?pcm=1&sr=16000`
- 客户端 → 服务端：二进制帧 = 16kHz 单声道 s16le PCM
- 服务端 → 客户端（JSON 文本帧）：
  - `{"type":"session.started","protocol":"voicestudio.speech.v1","session_id":"..."}`
  - `{"type":"partial","text":"...","session_id":"..."}`
  - `{"type":"final","final_kind":"summary","text":"...","session_id":"..."}`
- 结束：发送 `{"type":"input_audio.end"}`（不必关闭 socket）

> 本项目当前是"说完再识别"（batch），暂不消费该接口；保留后便于后续升级为实时字幕/低延迟。

### 3.2 健康检查（建议提供）

`GET /health` 返回 200（原版的 `GET /.well-known/voicestudio-speech` 也可以）。

---

## 4. 本项目如何接入（我方操作）

### 4.1 修改 `server/.env`

```dotenv
XZ_ASR_PROVIDER=openai
XZ_ASR_BASE_URL=http://127.0.0.1:3900/v1
XZ_ASR_API_KEY=             # 本地服务留空即可（已支持免 Key）；如需鉴权填 key
XZ_ASR_MODEL=whisper        # 服务端忽略即可，随便填
XZ_ASR_LANGUAGE=zh
```

然后重启服务端（`cd server` + `.\run.ps1`，或 `.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 0.0.0.0 --port 8000`）。

### 4.2 我方调用流程（供双方对齐）

1. 设备上行 Opus 音频 → 我方解码为 PCM（16k mono）
2. 打包为 **16kHz 单声道 16-bit WAV**
3. `POST {XZ_ASR_BASE_URL}/audio/transcriptions`，multipart 表单：`file` / `model` / `language` / `response_format=json`
4. 若配置了 `XZ_ASR_API_KEY`，带 `Authorization: Bearer <key>` 请求头
5. 取响应 JSON 的 `text` 字段作为识别结果

### 4.3 验证步骤

1. 先按 §2.4 用 curl 单独确认接口可用；
2. 启动我方服务端，日志应出现：`ASR provider: openai-compatible (http://127.0.0.1:3900/v1)`；
3. 开发板完整对话一轮：屏幕应显示"你说的话"（即同事服务的识别结果）。

### 4.4 出问题时的降级 / 其它部署方式

- **随时可降级**：把 `XZ_ASR_PROVIDER` 改回 `local` 即恢复本机 faster-whisper（依赖已装好）。
- **服务在另一台机器**：`XZ_ASR_BASE_URL=http://<对方IP>:3900/v1`，对方需放行防火墙；若有鉴权，填 `XZ_ASR_API_KEY`。
- **端口不是 3900**：改 `BASE_URL` 即可（路径必须以 `/v1` 结尾，因为客户端会拼 `/audio/transcriptions`）。

---

## 5. 边界与约定

- **并发**：本场景低负载（单设备，同一时刻最多 1 个转写请求）。
- **隐私**：音频仅在本机/局域网内传输，不出公网。
- **许可**：同事的服务是 VoiceStudio（AGPL-3.0）的衍生物——双方只通过 HTTP 接口对接没有问题；**不要把其源码并入本项目仓库**（GPL 传染性）。
- **模型名**：无论服务端实际用什么引擎（Sherpa / Whisper / OmniVoice），请对 `model` 字段保持"接受并忽略"，这样双方接口最稳定。

---

## 附：如果同事的接口不是 OpenAI 兼容格式

请尽量向上面的 OpenAI 兼容格式靠拢（这是事实标准，改动最小）。
若无法兼容，请把实际接口（路径、请求字段、响应结构、音频格式）发回来，我在 `server/app/providers/asr.py` 里新增一个 provider（约 30 行）来适配，同样不用动其余逻辑。

---

## 附二（推荐）：给同事的最小兼容路由示例（约 15 行，建议实现）

在现有 FastAPI 服务里**复用现有转录逻辑**，只加一个 OpenAI 兼容的别名路由（内部实现完全不动）：

```python
@app.post("/v1/audio/transcriptions")
async def openai_compat(
    file: UploadFile = File(...),
    model: str = Form("whisper"),          # 接受并忽略
    language: str = Form("zh"),
    response_format: str = Form("json"),   # 接受并忽略
):
    # 直接调用你们现有 /transcribe 的处理函数（函数名按实际代码改）
    result = await transcribe_audio(file=file, language=language)
    return {"text": (result.get("result") or {}).get("text", "")}
```

加好后，我方切换到**通用通道**（零代码改动）：

```dotenv
XZ_ASR_PROVIDER=openai
XZ_ASR_BASE_URL=http://192.168.5.102:7778/v1
```

**好处**
- 对我方（TK 助手中台）：从此接任何 OpenAI 兼容 ASR（OpenAI / 硅基流动 / Groq / LM Studio / whisper.cpp server …）都只改 `.env`，不再为每家写适配器
- 对同事：`/v1/audio/transcriptions` 是事实标准，他的服务从此能被任何 OpenAI 兼容客户端使用，更通用
- 现有 `/transcribe` 路由保留不动，向前兼容；我方已有的 `voicestudio` provider 也保留作兜底

**验证**：加好路由后执行
```powershell
curl.exe -X POST http://192.168.5.102:7778/v1/audio/transcriptions `
  -F "file=@server\tools\asr_test_zh.wav;type=audio/wav" -F "model=x" -F "language=zh" -F "response_format=json"
# 期望：{"text":"你好小智,今天天气怎么样?"}
```
