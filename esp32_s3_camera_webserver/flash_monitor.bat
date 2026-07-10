@echo off
if "%~1"=="" (
    echo Usage: flash_monitor.bat COMx
    exit /b 1
)
call "%~dp0idf_env.bat" -p %1 flash monitor
