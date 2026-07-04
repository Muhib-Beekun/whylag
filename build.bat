@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem Stop running builds from locking the output exe
taskkill /F /IM whylag-gui.exe >nul 2>&1
taskkill /F /IM whylag.exe >nul 2>&1
taskkill /F /IM lagmon.exe >nul 2>&1

if not exist ".temp\build" mkdir ".temp\build"

rem Move local scratch out of repo root when present
if exist "iterate.bat" move /Y "iterate.bat" ".temp\build\" >nul
if exist "lagmon.exe" del /F /Q "lagmon.exe" >nul
for %%F in ("Screenshot*.png") do if exist "%%~F" move /Y "%%~F" ".temp\" >nul

set "GCC="

where gcc >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%G in ('where gcc 2^>nul') do (
        set "GCC=%%G"
        goto :found
    )
)

rem WinGet LLVM-MinGW (user install)
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.*") do (
    for /d %%R in ("%%D\llvm-mingw-*") do (
        if exist "%%R\bin\gcc.exe" (
            set "GCC=%%R\bin\gcc.exe"
            goto :found
        )
    )
)

rem Chocolatey / MSYS64 / legacy paths
for %%P in (
    "C:\ProgramData\chocolatey\lib\mingw\tools\install\mingw64\bin\gcc.exe"
    "C:\msys64\mingw64\bin\gcc.exe"
    "C:\msys64\ucrt64\bin\gcc.exe"
    "C:\MinGW\bin\gcc.exe"
    "C:\mingw64\bin\gcc.exe"
) do (
    if exist %%P (
        set "GCC=%%~P"
        goto :found
    )
)

echo error: gcc not found.
echo.
echo Install one of:
echo   winget install MartinStorsjo.LLVM-MinGW.UCRT
echo   choco install mingw
echo.
echo Or add your MinGW bin folder to PATH, then re-run build.bat
exit /b 1

:found
echo Using !GCC!
set "BINDIR=!GCC:\gcc.exe=!"
set "PATH=!BINDIR!;!PATH!"

echo Generating icon (optional) ...
python "tools\make_icon.py" 2>nul
if errorlevel 1 if not exist "whylag.ico" (
    echo error: whylag.ico missing and could not run tools\make_icon.py
    echo Install Pillow: pip install pillow
    exit /b 1
)

echo Compiling resources ...
set "WINDRES=!BINDIR!\windres.exe"
if not exist "!WINDRES!" set "WINDRES=windres"
"!WINDRES!" whylag.rc -O coff -o ".temp\build\whylag_res.o"
if errorlevel 1 exit /b 1

echo Building whylag.exe ...
"!GCC!" -O2 -Wall -o whylag.exe whylag.c whylag_core.c ".temp\build\whylag_res.o" -ltdh -ladvapi32 -lshell32
if errorlevel 1 exit /b 1

echo Building whylag-gui.exe ...
"!GCC!" -O2 -Wall -o ".temp\build\whylag-gui.exe" whylag_gui.c whylag_gui_theme.c whylag_help.c whylag_detail.c whylag_settings_dlg.c whylag_core.c ".temp\build\whylag_res.o" -ltdh -ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -ldwmapi -luxtheme -lshell32 -mwindows
if errorlevel 1 exit /b 1

copy /Y ".temp\build\whylag-gui.exe" "whylag-gui.exe" >nul 2>&1
if errorlevel 1 (
    echo.
    echo Note: could not overwrite whylag-gui.exe ^(likely running elevated^).
    echo Built copy: .temp\build\whylag-gui.exe
    echo Close the app and re-run build, or run the copy from .temp\build\.
) else (
    echo Installed whylag-gui.exe
)

echo Done.
endlocal
