@echo off
setlocal
rem cmd.exe cannot use a UNC path (\\server\share\...) as its working directory and
rem silently falls back to C:\Windows; pushd maps a temporary drive letter for it.
pushd "%~dp0"
rem BennuGD for MiSTer - Quartus Prime 17.0 Lite command-line build.
rem Usage: build.bat            (full compile, copies dated .rbf to releases\)
if "%QUARTUS_ROOTDIR%"=="" set QUARTUS_ROOTDIR=C:\intelFPGA_lite\17.0\quartus
set PATH=%QUARTUS_ROOTDIR%\bin64;%PATH%
set PROJECT=BennuGD

quartus_sh --flow compile %PROJECT%
if errorlevel 1 (
    echo.
    echo BUILD FAILED - last lines of the flow log:
    for %%f in (output_files\%PROJECT%.map.rpt output_files\%PROJECT%.fit.rpt) do (
        if exist %%f findstr /i /c:"Error" %%f
    )
    popd
    exit /b 1
)

set STAMP=
rem worst timing paths in a text file (readable from the Mac share)
quartus_sta -t tools\sta_paths.tcl %PROJECT%

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd"') do set STAMP=%%i
if "%STAMP%"=="" for /f "tokens=2 delims==" %%i in ('wmic os get localdatetime /value') do set STAMP=%%i
if "%STAMP%"=="" set STAMP=build
set STAMP=%STAMP:~0,8%
if not exist releases mkdir releases
copy /y output_files\%PROJECT%.rbf releases\%PROJECT%_%STAMP%.rbf
if errorlevel 1 (
    echo COPY FAILED - the .rbf is still at output_files\%PROJECT%.rbf
    popd
    exit /b 1
)
echo.
findstr /c:"Worst-case setup slack" output_files\%PROJECT%.sta.rpt
findstr /c:"Logic utilization" output_files\%PROJECT%.fit.summary
findstr /c:"Total block memory bits" output_files\%PROJECT%.fit.summary
rem the fitter's hard limit is the block count, not the bits (8/16-bit wide RAMs leave a fifth of each block idle)
findstr /r /c:"^; M10K blocks" output_files\%PROJECT%.fit.rpt
echo.
echo Done: releases\%PROJECT%_%STAMP%.rbf
endlocal
popd
