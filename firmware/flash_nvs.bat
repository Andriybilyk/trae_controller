@echo off
echo ========================================================
echo   Flashing NVS Backup (Touch Calibration ^& WiFi)
echo ========================================================
echo.
echo Make sure your ESP32 is connected.
set "PORT=%~1"
if "%PORT%"=="" set /p PORT="Enter COM port (e.g., COM8): "

if "%PORT%"=="" (
    echo Error: COM port is required.
    exit /b 1
)

echo Flashing NVS to %PORT%...
set "EXPORT_BAT="
if not "%IDF_PATH%"=="" (
  if exist "%IDF_PATH%\export.bat" set "EXPORT_BAT=%IDF_PATH%\export.bat"
)

if "%EXPORT_BAT%"=="" if exist "C:\esp\.espressif\v6.0\esp-idf\export.bat" set "EXPORT_BAT=C:\esp\.espressif\v6.0\esp-idf\export.bat"
if "%EXPORT_BAT%"=="" if exist "C:\esp\v6.0\esp-idf\export.bat" set "EXPORT_BAT=C:\esp\v6.0\esp-idf\export.bat"
if "%EXPORT_BAT%"=="" if exist "C:\Espressif\frameworks\esp-idf-v6.0\export.bat" set "EXPORT_BAT=C:\Espressif\frameworks\esp-idf-v6.0\export.bat"
if "%EXPORT_BAT%"=="" if exist "C:\Espressif\frameworks\esp-idf-v5.5.3\export.bat" set "EXPORT_BAT=C:\Espressif\frameworks\esp-idf-v5.5.3\export.bat"
if "%EXPORT_BAT%"=="" if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" set "EXPORT_BAT=C:\Espressif\frameworks\esp-idf-v5.5\export.bat"
if "%EXPORT_BAT%"=="" if exist "C:\esp\v5.5.3\esp-idf\export.bat" set "EXPORT_BAT=C:\esp\v5.5.3\esp-idf\export.bat"

if "%EXPORT_BAT%"=="" (
  for /f "delims=" %%i in ('dir /b /s "C:\Espressif\*\export.bat" 2^>nul') do (
    set "EXPORT_BAT=%%i"
    goto :export_found
  )
)
:export_found

if "%EXPORT_BAT%"=="" (
  echo ESP-IDF export.bat not found.
  echo Install ESP-IDF 6.x/5.x and/or set IDF_PATH, then retry.
  exit /b 1
)

call "%EXPORT_BAT%"
esptool.py -p %PORT% -b 460800 write_flash 0x9000 nvs_backup.bin

echo.
echo Sending explicit RUN reset...
python -c "import serial; s=serial.Serial('%PORT%',115200,timeout=0.2); s.dtr=False; s.rts=False; s.close()" >nul 2>nul
python -m esptool --chip esp32s3 -p %PORT% --after hard_reset run

echo.
echo Done! Please restart your device.
pause
