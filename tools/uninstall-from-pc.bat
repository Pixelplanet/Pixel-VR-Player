@echo off
setlocal enabledelayedexpansion

echo ===============================================================
echo        Pixel VR Player - Steam Frame Remote Uninstaller
echo ===============================================================
echo  This tool removes Pixel VR Player and its Steam shortcut from
echo  your Valve Steam Frame over your local Wi-Fi network.
echo ===============================================================
echo.

set "FRAME_IP=steamframe.local"
set /p "INPUT_IP=Enter Steam Frame IP address [press Enter for steamframe.local]: "
if not "!INPUT_IP!"=="" set "FRAME_IP=!INPUT_IP!"

echo.
echo Connecting to steamos@!FRAME_IP!...
echo.

where ssh >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] SSH is not installed or not in PATH on this PC.
    pause
    exit /b 1
)

ssh -o StrictHostKeyChecking=no steamos@!FRAME_IP! "curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh -s -- --uninstall"

if %ERRORLEVEL% equ 0 (
    echo.
    echo ===============================================================
    echo  [SUCCESS] Pixel VR Player removed from your Steam Frame!
    echo ===============================================================
) else (
    echo.
    echo ===============================================================
    echo  [NOTE] If connection failed, please ensure:
    echo   1. Both your PC and Steam Frame are on the same Wi-Fi.
    echo   2. SSH is enabled on the headset.
    echo ===============================================================
)

echo.
pause
