@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1"
if errorlevel 1 (
    echo.
    echo Build failed. Read the error above and README.md for prerequisites.
    pause
    exit /b 1
)
echo.
echo Ready. Open BLACK PC Launcher.exe to play or BLACK PC Settings.exe to configure.
pause
