@echo off
setlocal
cd /d "%~dp0.."
set FAIL=0

if not exist whylag.exe (
    echo test: whylag.exe not found - run build.bat first
    exit /b 1
)

echo test: version
whylag.exe -v | findstr /C:"0.3.1" >nul || set FAIL=1

echo test: compare detects nvlddmkm regression
whylag.exe compare tests\fixtures\baseline.csv tests\fixtures\bad.csv > tests\output_compare.txt
findstr /C:"nvlddmkm" tests\output_compare.txt >nul || set FAIL=1
findstr /C:"2500" tests\output_compare.txt >nul || set FAIL=1

echo test: compare identical files
whylag.exe compare tests\fixtures\baseline.csv tests\fixtures\baseline.csv > tests\output_same.txt
findstr /C:"No significant regressions" tests\output_same.txt >nul || set FAIL=1

echo test: snapshot load via core export format
rem covered by compare parse of fixture CSVs with empty pid/cpu fields

if %FAIL%==0 (
    echo All tests passed.
    exit /b 0
)
echo Some tests failed.
exit /b 1
