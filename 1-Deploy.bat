@echo off
setlocal
cd /d "%~dp0"

cmake -S RenderingPlugin -B _Build -G "Visual Studio 17 2022" -A x64 %*
exit /B %ERRORLEVEL%
