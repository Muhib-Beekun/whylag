@echo off
setlocal
cd /d "%~dp0"

where gcc >nul 2>&1
if errorlevel 1 (
    echo error: gcc not found. Install MinGW and add it to PATH.
    exit /b 1
)

echo Building whylag.exe ...
gcc -O2 -Wall -o whylag.exe whylag.c whylag_core.c -ltdh -ladvapi32
if errorlevel 1 exit /b 1

echo Building whylag-gui.exe ...
gcc -O2 -Wall -o whylag-gui.exe whylag_gui.c whylag_core.c -ltdh -ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -mwindows
if errorlevel 1 exit /b 1

echo Done.
endlocal
