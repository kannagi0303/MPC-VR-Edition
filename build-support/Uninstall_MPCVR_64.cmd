@echo off
setlocal
set "ax=%~dp0MPCVRMFG64.ax"
set "regsvr=%SystemRoot%\System32\regsvr32.exe"
if exist "%SystemRoot%\Sysnative\regsvr32.exe" set "regsvr=%SystemRoot%\Sysnative\regsvr32.exe"

if not exist "%ax%" goto missing
"%regsvr%" /u /s "%ax%"
if errorlevel 1 goto failed

echo.
echo    Uninstallation succeeded.
echo.
goto done

:missing
echo.
echo    Uninstallation failed: MPCVRMFG64.ax not found next to this script.
echo.
goto error

:failed
echo.
echo    Uninstallation failed.
echo    Right-click Uninstall_MPCVR_64.cmd and choose "Run as administrator".
echo.
:error
pause >nul
exit /b 1

:done
pause >nul
exit /b 0
