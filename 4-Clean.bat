@echo off
setlocal
cd /d "%~dp0"

if exist "_Bin" rd /q /s "_Bin"
if exist "_Build" rd /q /s "_Build"
if exist "Build" rd /q /s "Build"
if exist "RenderingPlugin\build" rd /q /s "RenderingPlugin\build"
if exist "RenderingPlugin\out" rd /q /s "RenderingPlugin\out"
