$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

if (-not (Test-Path ".venv")) {
    Write-Host "[setup] creating virtual environment ..."
    python -m venv .venv
}

Write-Host "[setup] installing dependencies ..."
& ".venv\Scripts\python.exe" -m pip install -q -r requirements.txt

if (-not (Test-Path ".env")) {
    Copy-Item ".env.example" ".env"
    Write-Host "[setup] created .env - 请填入 XZ_LLM_API_KEY 后再运行"
}

Write-Host "[run] starting xiaozhi server on http://0.0.0.0:8000 ..."
& ".venv\Scripts\python.exe" -m uvicorn app.main:app --host 0.0.0.0 --port 8000
