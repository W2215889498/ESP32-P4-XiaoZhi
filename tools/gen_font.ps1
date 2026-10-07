# 生成 main/assets/font_tk_16.c（LVGL 点阵字库，全量中文，4bpp 无压缩）
#
# 覆盖范围：ASCII + Latin-1 + 常用标点 + 全角字符 + 全部 CJK 汉字（U+4E00..U+9FFF）
# 字体：Source Han Sans SC Regular（OFL 开源），脚本会自动下载到 %TEMP%
# 依赖：Node.js（npx 会自动下载 lv_font_conv）
#
# 用法： powershell -ExecutionPolicy Bypass -File tools\gen_font.ps1
# 生成后重新编译：idf.py build

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$font = Join-Path $env:TEMP "SourceHanSansSC-Regular.otf"
if (-not (Test-Path $font)) {
    Write-Output "downloading SourceHanSansSC-Regular.otf ..."
    Invoke-WebRequest `
        "https://cdn.jsdelivr.net/gh/adobe-fonts/source-han-sans@release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf" `
        -OutFile $font
}

$env:npm_config_registry = "https://registry.npmmirror.com"

npx --yes lv_font_conv@1.5.2 `
  --font $font `
  --size 16 --bpp 4 --no-compress --no-prefilter --no-kerning `
  --range 0x20-0x7F,0xA0-0xFF,0x2010-0x206F,0x2100-0x214F,0x2190-0x2199,0x3000-0x303F,0x4E00-0x9FFF,0xFF00-0xFFEF `
  --format lvgl --lv-include "lvgl.h" `
  -o "$root\main\assets\font_tk_16.c"

Write-Output ("generated: " + (Get-Item "$root\main\assets\font_tk_16.c").FullName)
