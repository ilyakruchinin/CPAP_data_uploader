@echo off
REM ESP32 Standard Firmware Initial Flash Script for Windows using esptool

setlocal enabledelayedexpansion

REM Check if port is provided
if "%~1"=="" (
    echo ========================================
    echo ESP32 Standard Firmware Upload Tool
    echo ========================================
    echo.
    echo Error: Serial port not specified
    echo.
    echo Usage: %~nx0 ^<COM_PORT^>
    echo.
    echo Example: %~nx0 COM3
    echo.
    echo ========================================
    echo How to find your COM port:
    echo ========================================
    echo.
    echo 1. Open Device Manager
    echo    - Press Win+X and select "Device Manager"
    echo    - Or search for "Device Manager" in Start menu
    echo.
    echo 2. Expand "Ports (COM ^& LPT^)"
    echo.
    echo 3. Look for one of these:
    echo    - USB-SERIAL CH340 (COMx^)
    echo    - Silicon Labs CP210x USB to UART Bridge (COMx^)
    echo    - USB Serial Port (COMx^)
    echo.
    echo 4. Note the COM port number (e.g., COM3, COM4, COM5^)
    echo.
    echo 5. Run this script again with your COM port:
    echo    %~nx0 COM3
    echo.
    echo ========================================
    echo.
    pause
    exit /b 1
)

set "PORT=%~1"
set "CHIP=esp32"
set "BAUD_RATE=460800"
set "FIRMWARE_FILE=firmware-standard.bin"

echo.
echo ========================================
echo ESP32 Standard Firmware Upload
echo ========================================
echo Port: !PORT!
echo Firmware: Standard (3MB app space, no OTA)
echo Baud rate: !BAUD_RATE!
echo.

REM Check if Python is installed
python --version >nul 2>&1
if errorlevel 1 (
    echo Error: Python is not installed or not in PATH
    echo.
    echo Please install Python 3.7 or later from:
    echo https://www.python.org/downloads/
    echo.
    echo Make sure to check "Add Python to PATH" during installation
    pause
    exit /b 1
)

REM Check if firmware file exists
if not exist "!FIRMWARE_FILE!" (
    echo Error: !FIRMWARE_FILE! not found
    echo.
    echo Make sure you are running this script from the release package directory
    pause
    exit /b 1
)

REM Check if virtual environment exists, create if not
if not exist ".venv\" (
    echo Creating virtual environment...
    python -m venv .venv
    if errorlevel 1 (
        echo Error: Failed to create virtual environment
        pause
        exit /b 1
    )
)

REM Activate virtual environment
call .venv\Scripts\activate.bat
if errorlevel 1 (
    echo Error: Failed to activate virtual environment
    pause
    exit /b 1
)

REM Check if esptool is installed, install if not
python -m esptool version >nul 2>&1
if errorlevel 1 (
    echo Installing esptool...
    python -m pip install esptool
    if errorlevel 1 (
        echo Error: Failed to install esptool
        pause
        exit /b 1
    )
)

REM Upload firmware using esptool
echo Uploading firmware...
echo.

python -m esptool --chip !CHIP! --port !PORT! --baud !BAUD_RATE! ^
    --before default_reset --after hard_reset write_flash -z ^
    0x0 !FIRMWARE_FILE!

if errorlevel 1 (
    echo.
    echo ========================================
    echo Upload failed!
    echo ========================================
    echo.
    echo Troubleshooting:
    echo   1. Make sure the ESP32 is connected
    echo   2. Try holding the BOOT button during upload
    echo   3. Check Device Manager to verify the correct COM port
    echo   4. Close any programs using the serial port
    echo.
    pause
    exit /b 1
) else (
    echo.
    echo ========================================
    echo Upload successful!
    echo ========================================
    echo.
    pause
)
