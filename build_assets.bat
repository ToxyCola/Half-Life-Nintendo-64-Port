@echo off
setlocal
set "HALF_LIFE_VALVE=E:\MYPC\Steam\steamapps\common\Half-Life\valve"
set "N64_INST=C:\n64-toolchain"
set "PATH=C:\n64-toolchain\bin;%PATH%"
set "PYTHONUTF8=1"
set "PYTHONUNBUFFERED=1"

set ARGS=
:arg_loop
if "%~1"=="" goto arg_done
if exist "%~1\maps\c1a0.bsp" (
    set "HALF_LIFE_VALVE=%~1"
) else (
    set "ARGS=%ARGS% %1"
)
shift
goto arg_loop
:arg_done

if not exist "%HALF_LIFE_VALVE%\maps\c1a0.bsp" (
    echo Error: Could not find Half-Life data at %HALF_LIFE_VALVE%
    echo Please pass the path to your Half-Life valve directory as an argument.
    exit /b 1
)

echo ========================================================
echo Converting Half-Life assets for Nintendo 64
echo Source: %HALF_LIFE_VALVE%
echo Output: filesystem
echo ========================================================

python tools\build_assets.py "%HALF_LIFE_VALVE%" filesystem %ARGS%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo Asset conversion finished!
    echo Now run build_n64.bat to compile the final ROM.
    echo ========================================================
) else (
    echo.
    echo Asset conversion encountered an error (%ERRORLEVEL%).
)
endlocal
