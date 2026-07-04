@echo off
setlocal
cd /d "%~dp0.."

set "RES_OBJ=%~1"
set "CLI_OUT=%~2"
set "GUI_OUT=%~3"

if "%RES_OBJ%"=="" (
    echo usage: build-binaries.bat RES_OBJ CLI_OUT GUI_OUT
    exit /b 1
)
if "%CLI_OUT%"=="" (
    echo usage: build-binaries.bat RES_OBJ CLI_OUT GUI_OUT
    exit /b 1
)
if "%GUI_OUT%"=="" (
    echo usage: build-binaries.bat RES_OBJ CLI_OUT GUI_OUT
    exit /b 1
)

for %%D in ("%RES_OBJ%" "%CLI_OUT%" "%GUI_OUT%") do (
    if not exist "%%~dpD" mkdir "%%~dpD"
)

set "GUI_MAN_OBJ=%RES_OBJ:_res.o=_gui_manifest.o%"
if /I "%GUI_MAN_OBJ%"=="%RES_OBJ%" set "GUI_MAN_OBJ=%~dp1whylag_gui_manifest.o"

windres whylag.rc -O coff -o "%RES_OBJ%"
if errorlevel 1 exit /b 1

windres whylag_gui_manifest.rc -O coff -o "%GUI_MAN_OBJ%"
if errorlevel 1 exit /b 1

gcc -O2 -Wall -o "%CLI_OUT%" whylag.c whylag_core.c "%RES_OBJ%" -ltdh -ladvapi32 -lshell32
if errorlevel 1 exit /b 1

gcc -O2 -Wall -o "%GUI_OUT%" whylag_gui.c whylag_gui_theme.c whylag_help.c whylag_detail.c whylag_settings_dlg.c whylag_core.c "%RES_OBJ%" "%GUI_MAN_OBJ%" -ltdh -ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -ldwmapi -luxtheme -lshell32 -mwindows
if errorlevel 1 exit /b 1

endlocal
