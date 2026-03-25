@echo off
setlocal
echo ==========================================
echo   Flash firmware and force normal boot
echo ==========================================
echo.

set "IDF_PATH_ARG=%~2"
if not "%IDF_PATH_ARG%"=="" set "IDF_PATH=%IDF_PATH_ARG%"

if "%IDF_PATH%"=="" if exist "C:\esp\.espressif\v6.0\esp-idf\export.bat" set "IDF_PATH=C:\esp\.espressif\v6.0\esp-idf"
if "%IDF_PATH%"=="" if exist "C:\esp\v6.0\esp-idf\export.bat" set "IDF_PATH=C:\esp\v6.0\esp-idf"
if "%IDF_PATH%"=="" if exist "C:\esp\v5.5.3\esp-idf\export.bat" set "IDF_PATH=C:\esp\v5.5.3\esp-idf"
if "%IDF_PATH%"=="" if exist "C:\Espressif\frameworks\esp-idf-v5.5.3\export.bat" set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.3"
if "%IDF_PATH%"=="" if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5"

set "PORT=%~1"
if "%PORT%"=="" set /p PORT=Enter COM port (e.g. COM8): 
if "%PORT%"=="" (
  echo COM port is required.
  exit /b 1
)

set "EXPORT_PS1="
if not "%IDF_PATH%"=="" (
  if exist "%IDF_PATH%\export.ps1" set "EXPORT_PS1=%IDF_PATH%\export.ps1"
)

if "%EXPORT_PS1%"=="" if exist "C:\esp\.espressif\v6.0\esp-idf\export.ps1" set "EXPORT_PS1=C:\esp\.espressif\v6.0\esp-idf\export.ps1"
if "%EXPORT_PS1%"=="" if exist "C:\esp\v6.0\esp-idf\export.ps1" set "EXPORT_PS1=C:\esp\v6.0\esp-idf\export.ps1"
if "%EXPORT_PS1%"=="" if exist "C:\Espressif\frameworks\esp-idf-v6.0\export.ps1" set "EXPORT_PS1=C:\Espressif\frameworks\esp-idf-v6.0\export.ps1"
if "%EXPORT_PS1%"=="" if exist "C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1" set "EXPORT_PS1=C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1"
if "%EXPORT_PS1%"=="" if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.ps1" set "EXPORT_PS1=C:\Espressif\frameworks\esp-idf-v5.5\export.ps1"
if "%EXPORT_PS1%"=="" if exist "%USERPROFILE%\esp\esp-idf-v6.0\export.ps1" set "EXPORT_PS1=%USERPROFILE%\esp\esp-idf-v6.0\export.ps1"
if "%EXPORT_PS1%"=="" if exist "%USERPROFILE%\esp\esp-idf-v5.5.3\export.ps1" set "EXPORT_PS1=%USERPROFILE%\esp\esp-idf-v5.5.3\export.ps1"

if "%EXPORT_PS1%"=="" (
  echo ESP-IDF export.ps1 not found.
  echo Install ESP-IDF 6.x/5.x and/or set IDF_PATH, then retry.
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%EXPORT_PS1%'; Set-Location '%~dp0'; idf.py set-target esp32s3; if ($LASTEXITCODE -ne 0) { exit 1 }; idf.py build; if ($LASTEXITCODE -ne 0) { exit 1 }; idf.py -p %PORT% flash; if ($LASTEXITCODE -ne 0) { exit 1 }; python -c \"import serial; s=serial.Serial('%PORT%',115200,timeout=0.2); s.dtr=False; s.rts=False; s.close()\" | Out-Null; python -m esptool --chip esp32s3 -p %PORT% --after hard_reset run" 
if errorlevel 1 exit /b 1

echo.
echo Done. Device should boot in normal mode now.
endlocal
