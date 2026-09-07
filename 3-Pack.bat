@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release

set BIN=%cd%\_Bin\%CONFIG%
set SRC=%cd%\Packages\top.kuanmi.unityrhi.native
set DST=%cd%\Build\top.kuanmi.unityrhi.native
set PLUGINS=%DST%\Plugins\x86_64

if not exist "%BIN%\UnityRHI.dll" (
    echo Missing "%BIN%\UnityRHI.dll"
    echo Run 2-Build.bat %CONFIG% first.
    exit /B 1
)

echo Packing native UPM into "%DST%"

if exist "%DST%" rd /q /s "%DST%"
mkdir "%DST%" >nul

robocopy "%SRC%" "%DST%" /E /NFL /NDL /NJH /NJS /nc /ns /np /XF *.dll *.pdb >nul
if %ERRORLEVEL% GEQ 8 exit /B 1

if not exist "%PLUGINS%\D3D12" mkdir "%PLUGINS%\D3D12"

call :copy_required "%BIN%\UnityRHI.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\NRI.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\NRIPlugin.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\D3D12HeapHook.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\NRD.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\dxcompiler.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\dxil.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\nvngx_dlss.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\nvngx_dlssd.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\nvngx_dlssg.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_optional "%BIN%\nvngx_dlssnr.dll" "%PLUGINS%\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\AgilitySDK\D3D12Core.dll" "%PLUGINS%\D3D12\"
if errorlevel 1 exit /B 1
call :copy_required "%BIN%\AgilitySDK\d3d12SDKLayers.dll" "%PLUGINS%\D3D12\"
if errorlevel 1 exit /B 1

if /I "%CONFIG%"=="Debug" (
    echo   Debug symbols:
    call :copy_optional "%BIN%\UnityRHI.pdb" "%PLUGINS%\"
    if errorlevel 1 exit /B 1
    call :copy_optional "%BIN%\NRI.pdb" "%PLUGINS%\"
    if errorlevel 1 exit /B 1
    call :copy_optional "%BIN%\NRIPlugin.pdb" "%PLUGINS%\"
    if errorlevel 1 exit /B 1
    call :copy_optional "%BIN%\D3D12HeapHook.pdb" "%PLUGINS%\"
    if errorlevel 1 exit /B 1
    call :copy_optional "%BIN%\NRD.pdb" "%PLUGINS%\"
    if errorlevel 1 exit /B 1
    call :copy_optional "%BIN%\AgilitySDK\D3D12Core.pdb" "%PLUGINS%\D3D12\"
    if errorlevel 1 exit /B 1
    call :copy_optional "%BIN%\AgilitySDK\d3d12SDKLayers.pdb" "%PLUGINS%\D3D12\"
    if errorlevel 1 exit /B 1
)

echo.
echo Packed native UPM:
echo   %DST%
echo.
echo Add the managed package to Packages/manifest.json:
echo   "top.kuanmi.unityrhi": "file:%cd:\=/%/Packages/top.kuanmi.unityrhi"
exit /B 0

:copy_required
if not exist "%~1" (
    echo Missing "%~1"
    exit /B 1
)
copy /Y "%~1" "%~2" >nul
if errorlevel 1 (
    echo Failed to copy "%~1" to "%~2"
    exit /B 1
)
echo   %~nx1
exit /B 0

:copy_optional
if not exist "%~1" (
    echo   %~nx1 ^(not configured^)
    exit /B 0
)
copy /Y "%~1" "%~2" >nul
if errorlevel 1 (
    echo Failed to copy "%~1" to "%~2"
    exit /B 1
)
echo   %~nx1
exit /B 0
