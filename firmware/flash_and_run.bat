@echo off
setlocal
echo ==========================================
echo   Flash firmware and force normal boot
echo ==========================================
echo.

set /p PORT=Enter COM port (e.g. COM8): 
if "%PORT%"=="" (
  echo COM port is required.
  exit /b 1
)

call C:\esp\v5.5.3\esp-idf\export.bat
if errorlevel 1 exit /b 1

idf.py -p %PORT% flash
if errorlevel 1 exit /b 1

echo.
echo Sending explicit RUN reset...
esptool.py -p %PORT% run
if errorlevel 1 exit /b 1

echo.
echo Done. Device should boot in normal mode now.
endlocal
