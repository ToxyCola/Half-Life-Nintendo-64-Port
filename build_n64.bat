@echo off
setlocal
echo ========================================
echo  Building Half-Life 64 with Libdragon
echo ========================================

set MSYS2_BASH=C:\msys64\usr\bin\bash.exe
if not exist "%MSYS2_BASH%" (
    echo Error: MSYS2 bash not found at %MSYS2_BASH%
    exit /b 1
)

if not exist "I:\tmp" mkdir "I:\tmp"
set TMP=I:\tmp
set TEMP=I:\tmp

"%MSYS2_BASH%" -lc "export MSYSTEM=MINGW64; . /etc/profile; export TMP=/i/tmp; export TEMP=/i/tmp; export TMPDIR=/i/tmp; mkdir -p /i/tmp; export N64_INST=/c/n64-toolchain; export PATH=$N64_INST/bin:$PATH; export T3D_INST=/home/dylan/tiny3d; cd /i/Half-Life-Nintendo-64-Port; mkdir -p filesystem; make -j$(nproc) %*"

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo  Build completed successfully!
    echo  ROM: hl64.z64
    echo ========================================
) else (
    echo.
    echo Build failed with error code %ERRORLEVEL%.
)
endlocal

