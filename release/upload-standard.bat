@echo off
REM ESP32 Standard Firmware Upload Script for Windows using PlatformIO

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
set "PIO_ENV=pico32"
set "FIRMWARE_DESC=Standard (3MB app space, no OTA)"

echo.
echo ========================================
echo ESP32 Standard Firmware Upload
echo ========================================
echo Port: !PORT!
echo Environment: !PIO_ENV!
echo Type: !FIRMWARE_DESC!
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

REM Check if PlatformIO is installed, install if not
python -m pip show platformio >nul 2>&1
if errorlevel 1 (
    echo Installing PlatformIO...
    python -m pip install platformio
    if errorlevel 1 (
        echo Error: Failed to install PlatformIO
        pause
        exit /b 1
    )
)

REM Upload firmware using PlatformIO
echo.
echo Uploading firmware...
echo.

pio run -e !PIO_ENV! -t upload --upload-port !PORT!

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
    echo   5. Make sure firmware was built first
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
