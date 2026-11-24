@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-support\export-project-for-gpt.ps1"
set "result=%errorlevel%"
echo.
pause
exit /b %result%
