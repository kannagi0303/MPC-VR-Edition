@echo off
setlocal
if not "%~1"=="" (
    echo Usage: build-x64.cmd
    exit /b 2
)
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0build-support\build-x64.ps1"
exit /b %ERRORLEVEL%
