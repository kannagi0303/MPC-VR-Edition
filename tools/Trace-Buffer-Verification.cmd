@echo off
setlocal DisableDelayedExpansion
rem [MFG-B10 | 2026-11-24 23:56 +08:00] Optional opt-in trace launcher.
rem Close existing MPC-BE instances first so the new process inherits the trace path.
set "player=%~1"
if defined player goto check
 echo Close all existing MPC-BE windows first.
 echo Drag the MPC-BE executable into this window and press Enter.
set /p "player=Player EXE: "
set "player=%player:"=%"
:check
if not exist "%player%" goto invalid
set "MPCVR_MFG_CADENCE_TRACE=%TEMP%\MPCVR-Buffer-Audit-%RANDOM%-%RANDOM%.csv"
echo Enable detailed Statistics in the renderer, reproduce the resize issue,
echo wait until the buffer refills, then close MPC-BE.
echo Log: "%MPCVR_MFG_CADENCE_TRACE%"
start "" "%player%"
echo Press any key after closing the player to locate the CSV.
pause >nul
if exist "%MPCVR_MFG_CADENCE_TRACE%" (explorer /select,"%MPCVR_MFG_CADENCE_TRACE%") else (echo No trace was written. Ensure a new MPC-BE process was launched.)
exit /b
:invalid
echo Executable not found.
pause
exit /b 1
