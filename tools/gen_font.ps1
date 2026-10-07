# 生成 main/assets/font_tk_16.bin（LVGL 二进制字库，全量中文）
#
# 覆盖范围：ASCII + Latin-1 + 常用标点 + 全角字符 + 全部 CJK 汉字（U+4E00..U+9FFF）
# 字体：Noto Sans SC（Windows 10/11 自带 NotoSansSC-VF.ttf，OFL 开源）
#      其他机器可改成任意 TTF/OTF 路径（如 SourceHanSansSC-Regular.otf）
# 依赖：Node.js（npx 会自动下载 lv_font_conv）
#
# 用法： powershell -ExecutionPolicy Bypass -File tools\gen_font.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$env:npm_config_registry = "https://registry.npmmirror.com"

New-Item -ItemType Directory -Force "$root\main\assets" | Out-Null

npx --yes lv_font_conv@1.5.2 `
  --font "C:\Windows\Fonts\NotoSansSC-VF.ttf" `
  --size 16 --bpp 4 --no-compress `
  --range 0x20-0x7F,0xA0-0xFF,0x2010-0x206F,0x2100-0x214F,0x2190-0x2199,0x3000-0x303F,0x4E00-0x9FFF,0xFF00-0xFFEF `
  --format bin -o "$root\main\assets\font_tk_16.bin"

Write-Output ("generated: " + (Get-Item "$root\main\assets\font_tk_16.bin").FullName)
