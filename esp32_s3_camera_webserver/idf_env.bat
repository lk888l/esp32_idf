@echo off
setlocal

set "ESP_IDF_ROOT=C:\kk_software\toolchain\esp_idf"
set "IDF_PATH=%ESP_IDF_ROOT%\frameworks\esp-idf-v5.5.4"

set "PATH=%ESP_IDF_ROOT%\python_env\idf5.5_py3.11_env\Scripts;%PATH%"
call "%IDF_PATH%\export.bat"

set "PATH=C:\kk_software\git\usr\bin;%ESP_IDF_ROOT%\python_env\idf5.5_py3.11_env\Scripts;%ESP_IDF_ROOT%\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;%ESP_IDF_ROOT%\tools\ninja\1.12.1;%ESP_IDF_ROOT%\tools\cmake\3.30.2\bin;%PATH%"

if "%~1"=="" (
    cmd /k
) else (
    python "%IDF_PATH%\tools\idf.py" -DCMAKE_MAKE_PROGRAM=%ESP_IDF_ROOT%/tools/ninja/1.12.1/ninja.exe %*
)
