# 服务端（TK助手后台）启动与运维指南

> 服务端位于仓库 `server/` 目录，基于 FastAPI + Uvicorn，监听 **0.0.0.0:8000**。
> 设备（ESP32-P4）启动后会自动从 `http://<电脑IP>:8000/xiaozhi/ota/` 拉配置并连上来。
>
> 本文档面向本机环境：`C:\Users\22158\Documents\esp32_projects\ESP32-P4-XiaoZhi`。

---

## 1. 一分钟启动（日常使用）

在项目根目录打开 **PowerShell**，运行：

```powershell
cd C:\Users\22158\Documents\esp32_projects\ESP32-P4-XiaoZhi\server
.\run.ps1
```

`run.ps1` 会自动完成：
1. 检查/创建虚拟环境 `.venv`（已存在则跳过）
2. 安装依赖（`requirements.txt`，已安装则很快跳过）
3. 若 `.env` 不存在则从 `.env.example` 复制（**首次必须填 Key，见 §3**）
4. 启动服务：`uvicorn app.main:app --host 0.0.0.0 --port 8000`

**看到这几行就是启动成功**：

```
xiaozhi-server 0.1.0
LLM : provider=deepseek model=deepseek-chat key=set
ASR : provider=local  model=...
TTS : provider=edge   voice=zh-CN-XiaoxiaoNeural
audio: libopus OK (Opus 16k/mono/60ms)
listening on http://0.0.0.0:8000  (ws path /xiaozhi/v1/)
Uvicorn running on http://0.0.0.0:8000
```

> 想敲命令的话：`.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 0.0.0.0 --port 8000`
>
> **更省事：直接双击 `server\run.bat`**（效果与 run.ps1 相同，永远使用项目 venv，避免"激活没生效"的坑）。

---

## 2. 启动后自检

浏览器打开（本机或同局域网手机都行）：

```
http://127.0.0.1:8000/health        # 本机自检
http://192.168.5.121:8000/health    # 手机验证（顺便验证防火墙）
```

期望返回（节选）：

```json
{ "opus": true,
  "llm": { "configured": true },
  "asr": { "provider": "local" },
  "tts": { "provider": "edge" } }
```

设备端的表现：屏幕状态从"获取服务端配置…"变为"就绪，点击按钮和我说话"，服务端日志出现：

```
config request from 192.168.5.193 -> ws://192.168.5.121:8000/xiaozhi/v1/
connected → hello → MCP ready, 1 tool(s): self.gpio.set_output
```

---

## 3. `.env` 配置（首次必做）

从 `.env.example` 复制一份（`run.ps1` 会自动做），然后编辑 `server\.env`：

| 变量 | 说明 | 本项目当前状态 |
|---|---|---|
| `XZ_LLM_API_KEY` | DeepSeek Key（**必填**，否则回复是 mock 提示语） | 已填 |
| `XZ_ASR_PROVIDER=local` | 本地识别，无需 Key（首次运行会自动下载模型） | 默认 |
| `XZ_TTS_PROVIDER=edge` | 免费微软语音；网络不稳可参考注释切硅基流动 | 默认 |
| `XZ_MCP_ENABLED=true` | 允许 AI 控制设备 GPIO | 默认 |

> ⚠️ `.env` 含密钥，**永远不要提交到 Git**（`.gitignore` 已忽略）。

---

## 4. 停止 / 重启

- **停止**：在运行服务的窗口按 `Ctrl+C`。
- **重启**：`Ctrl+C` 后重新运行 `run.ps1`。改了 `.env` 必须重启才生效。
- **端口被占用**（例如提示 `[Errno 10048]`）：

```powershell
# 查看谁占用了 8000
Get-NetTCPConnection -LocalPort 8000 -State Listen | Select-Object OwningProcess
# 结束该进程（把 <PID> 换成上面查到的）
Stop-Process -Id <PID> -Force
```

---

## 5. 开机自启动（可选）

如果想开机后服务端自动在后台跑（隐藏窗口），用任务计划程序注册一个登录任务：

```powershell
$ps = "C:\Users\22158\Documents\esp32_projects\ESP32-P4-XiaoZhi\server\run.ps1"
$action  = New-ScheduledTaskAction -Execute "powershell.exe" `
           -Argument "-WindowStyle Hidden -ExecutionPolicy Bypass -File `"$ps`""
$trigger = New-ScheduledTaskTrigger -AtLogOn
Register-ScheduledTask -TaskName "TKAssistantServer" -Action $action -Trigger $trigger -RunLevel Highest
```

- 查看/删除：
  ```powershell
  Get-ScheduledTask -TaskName "TKAssistantServer"
  Unregister-ScheduledTask -TaskName "TKAssistantServer" -Confirm:$false
  ```
- 日志查看（后台运行时没有窗口）：临时改成有窗口方式排查，或改用 `Start-Transcript` 输出到文件（可在 `run.ps1` 开头加 `Start-Transcript -Path "$here\server.log" -Append`）。

---

## 6. 常见问题速查

| 现象 | 原因 / 处理 |
|---|---|
| 设备显示"无法连接服务器，3 秒后重试" | ① 服务端没启动；② 防火墙未放行 8000（见下）；③ 设备里 OTA URL 的 IP 不是这台电脑的（`ipconfig` 确认后改 `sdkconfig.defaults.local` 重烧） |
| 设备连上但回复内容是"请把 DeepSeek 的 API Key 填进 .env" | LLM Key 没配或填错 → 改 `server\.env` 后重启 |
| 有回复文字但没声音 / 服务端日志 `TTS ... Connection timeout` | edge-tts 访问微软超时（常见于公司网络/aTrust VPN）。按 `.env.example` 注释配置硅基流动 TTS（`XZ_TTS_PROVIDER=openai` 等），或只填 `XZ_TTS_API_KEY` 自动兜底 |
| 识别不准 | `server\.env` 改 `XZ_ASR_LOCAL_MODEL=medium`（或 `large-v3`，需好显卡配置）后重启 |
| 首次启动很慢 | 本地 ASR 首次运行会从 `hf-mirror.com` 下载模型（small 约 460MB），之后启动很快 |
| 防火墙 | 管理员 PowerShell 执行一次：`New-NetFirewallRule -DisplayName "Xiaozhi Server 8000" -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow` |
| 日志出现"当前 Python 不是项目 venv"警告 | 启动时用的不是项目虚拟环境（常见原因：PowerShell 激活后命令解析仍指向全局 Python）。改用 `.\run.ps1` 或双击 `run.bat`；若同时提示缺依赖，按提示 `pip install` 即可 |

---

## 7. 与设备的联动关系（改 IP 时）

设备通过 `CONFIG_XIAOZHI_OTA_URL`（`http://<电脑IP>:8000/xiaozhi/ota/`）找到服务端。
电脑 IP 变化时需同步更新并重新烧录：

```ini
# 文件：sdkconfig.defaults.local（已被 .gitignore，本地私有）
CONFIG_XIAOZHI_OTA_URL="http://192.168.5.121:8000/xiaozhi/ota/"
```

然后（用 IDF 5.5.4 环境）重新编译烧录：`idf.py build` + `idf.py -p COM27 flash`。
