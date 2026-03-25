@echo off
setlocal
echo ==========================================
echo   Flash Web UI (LittleFS storage only)
echo ==========================================
echo.

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

powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%EXPORT_PS1%'; Set-Location '%~dp0'; idf.py build; if ($LASTEXITCODE -ne 0) { exit 1 }; idf.py -p %PORT% storage-flash" 
if errorlevel 1 exit /b 1

echo.
echo Done.
endlocal
