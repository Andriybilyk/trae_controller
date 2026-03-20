@echo off
echo ========================================================
echo   Flashing NVS Backup (Touch Calibration ^& WiFi)
echo ========================================================
echo.
echo Make sure your ESP32 is connected.
set /p PORT="Enter COM port (e.g., COM8): "

if "%PORT%"=="" (
    echo Error: COM port is required.
    exit /b 1
)

echo Flashing NVS to %PORT%...
call C:\esp\v5.5.3\esp-idf\export.bat
esptool.py -p %PORT% -b 460800 write_flash 0x9000 nvs_backup.bin

echo.
echo Done! Please restart your device.
pause