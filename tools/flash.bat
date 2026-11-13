@echo off
setlocal enabledelayedexpansion

set "DIR=%~dp0"
set "PORT=%~1"
set "PORT_ARGS="
if not "%PORT%"=="" set "PORT_ARGS=--port %PORT%"

set "ESPTOOL="
where esptool >nul 2>&1 && set "ESPTOOL=esptool"
if "%ESPTOOL%"=="" (where esptool.py >nul 2>&1 && set "ESPTOOL=esptool.py")
if "%ESPTOOL%"=="" (py -m esptool version >nul 2>&1 && set "ESPTOOL=py -m esptool")
if "%ESPTOOL%"=="" (python -m esptool version >nul 2>&1 && set "ESPTOOL=python -m esptool")
if "%ESPTOOL%"=="" (where uv >nul 2>&1 && set "ESPTOOL=uvx esptool")
if "%ESPTOOL%"=="" (
    echo error: esptool not found. Install it with: pip install esptool
    exit /b 1
)

echo Detecting ESP chip...
set "TMPFILE=%TEMP%\ows_chip_id_%RANDOM%.txt"
%ESPTOOL% %PORT_ARGS% chip_id > "%TMPFILE%" 2>&1
if errorlevel 1 (
    type "%TMPFILE%"
    del "%TMPFILE%" >nul 2>&1
    exit /b 1
)

set "CHIP="
set "PATTERN="
findstr /i "ESP32-S3" "%TMPFILE%" >nul && (set "CHIP=esp32s3" & set "PATTERN=ows-relay-esp32s3-full-*.bin")
if "%CHIP%"=="" (findstr /i "ESP32" "%TMPFILE%" >nul && (set "CHIP=esp32" & set "PATTERN=ows-relay-esp32-full-*.bin"))
if "%CHIP%"=="" (findstr /i "ESP8266" "%TMPFILE%" >nul && (set "CHIP=esp8266" & set "PATTERN=ows-relay-esp8266-*.bin"))
del "%TMPFILE%" >nul 2>&1

if "%CHIP%"=="" (
    echo error: unsupported chip ^(expected ESP32-S3, ESP32, or ESP8266^)
    exit /b 1
)

set "IMAGE="
for %%F in ("%DIR%%PATTERN%") do set "IMAGE=%%F"
if "%IMAGE%"=="" (
    echo error: no file matching %DIR%%PATTERN%
    exit /b 1
)

echo Flashing %CHIP% with %IMAGE%...
%ESPTOOL% --chip %CHIP% %PORT_ARGS% write_flash 0x0 "%IMAGE%"
