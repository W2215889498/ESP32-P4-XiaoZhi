# 可重复应用的原厂组件补丁（components 重新下载后运行本脚本恢复补丁）
#
# 补丁 1：esp_xiaozhi 的 websocket 任务栈 4KB -> 8KB
#   原因：MCP tools/list 响应在 websocket_task 上构建/打印时栈溢出
#   （Guru Meditation: Stack protection fault, task "websocket_task"）。
#   位置：managed_components/espressif__esp_xiaozhi/src/esp_xiaozhi_websocket.c
#
# 用法： powershell -ExecutionPolicy Bypass -File tools\apply_patches.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$f = Join-Path $root "managed_components\espressif__esp_xiaozhi\src\esp_xiaozhi_websocket.c"

if (-not (Test-Path $f)) {
    Write-Error "找不到 $f（先跑一次 idf.py build 让组件管理器下载依赖）"
}

$text = Get-Content $f -Raw
if ($text -match '\.task_stack\s*=\s*8192') {
    Write-Output "补丁已存在，跳过"
    exit 0
}

$old = @"
        .reconnect_timeout_ms = 5000,
    };
"@
$new = @"
        .reconnect_timeout_ms = 5000,
        // [PATCH-TK] 默认 4KB 不够：MCP tools/list 响应构建/打印会栈溢出
        // (Stack protection fault in websocket_task)。见 tools/apply_patches.ps1
        .task_stack = 8192,
    };
"@

if ($text.Contains($old)) {
    $text = $text.Replace($old, $new)
    Set-Content -Path $f -Value $text -NoNewline -Encoding UTF8
    Write-Output "已应用补丁: websocket task_stack = 8192"
} else {
    Write-Error "未找到补丁锚点（组件版本可能变了，请检查 esp_xiaozhi_websocket.c）"
}
