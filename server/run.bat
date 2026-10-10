@echo off
rem 一键启动 TK 助手服务端（使用项目 venv，双击即可）
cd /d "%~dp0"
powershell -ExecutionPolicy Bypass -File "%~dp0run.ps1"
pause
