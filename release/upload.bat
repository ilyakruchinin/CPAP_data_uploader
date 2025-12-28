@echo off
REM ESP32 Firmware Upload Script for Windows

setlocal enabledelayedexpansion

REM Configuration
set CHIP=esp32
set BAUD_RATE=460800
set FIRMWARE_OTA=firmware-ota.bin
set FIRMWARE_STANDARD=firmware-standard.bin
set FLASH_OFFSET=0x10000
set ESPTOOL=esptool.exe

REM Check if port is provided
if "%~1"=="" (
    echo ========================================
    echo ESP32 Firmware Upload Tool
    echo ========================================
    echo.
    echo Error: Serial port not specified
    echo.
    echo Usage: %~nx0 ^<COM_PORT^> [firmware_type]
    echo.
    echo Examples: 
    echo   %~nx0 COM3
    echo   %~nx0 COM3 ota
    echo   %~nx0 COM3 standard
    echo.
    echo Firmware options:
    echo   ^(default^)  - OTA firmware with web update capability
    echo   ota        - OTA firmware with web update capability  
    echo   standard   - Standard firmware ^(3MB app space, no OTA^)
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

set PORT=%~1
set FIRMWARE_TYPE=%~2

REM Default to OTA firmware if no type specified
if "%FIRMWARE_TYPE%"=="" set FIRMWARE_TYPE=ota

REM Select firmware file based on type
if /i "%FIRMWARE_TYPE%"=="ota" (
    if exist "%FIRMWARE_OTA%" (
        set FIRMWARE_FILE=%FIRMWARE_OTA%
    )
    set FIRMWARE_DESC=OTA-enabled ^(web updates supported^)
) else if /i "%FIRMWARE_TYPE%"=="standard" (
    if exist "%FIRMWARE_STANDARD%" (
        set FIRMWARE_FILE=%FIRMWARE_STANDARD%
    ) else (
        echo Error: Standard firmware file '%FIRMWARE_STANDARD%' not found
        pause
        exit /b 1
    )
    set FIRMWARE_DESC=Standard ^(3MB app space, no OTA^)
) else (
    echo Error: Invalid firmware type '%FIRMWARE_TYPE%'
    echo Valid options: ota, standard
    pause
    exit /b 1
)

REM Check if firmware file exists
if not exist "%FIRMWARE_FILE%" (
    echo Error: Firmware file '%FIRMWARE_FILE%' not found
    exit /b 1
)

REM Check if esptool.exe exists
if not exist "%ESPTOOL%" (
    echo Error: %ESPTOOL% not found in current directory
    pause
    exit /b 1
)

REM Upload firmware
echo.
echo ========================================
echo Uploading firmware to ESP32...
echo ========================================
echo Port: %PORT%
echo Firmware: %FIRMWARE_FILE%
echo Type: %FIRMWARE_DESC%
echo Baud rate: %BAUD_RATE%
echo.

%ESPTOOL% --chip %CHIP% --port %PORT% --baud %BAUD_RATE% write-flash %FLASH_OFFSET% %FIRMWARE_FILE%

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
