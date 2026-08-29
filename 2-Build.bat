@echo off
setlocal
cd /d "%~dp0"

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release

if not exist "_Build\CMakeCache.txt" (
    echo Run 1-Deploy.bat first.
    exit /B 1
)

cmake --build _Build --config %CONFIG% -j %NUMBER_OF_PROCESSORS%
exit /B %ERRORLEVEL%
