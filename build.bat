@echo off
REM Build the native Windows exe with the MinGW-w64 g++ already on PATH.
REM -static => no libgcc/libstdc++ DLL dependency; -s => strip; -mwindows => GUI subsystem.
setlocal
set OUT=AngryMoz.exe
windres app.rc -O coff -o app_res.o
if %ERRORLEVEL% NEQ 0 ( echo RESOURCE COMPILE FAILED & exit /b 1 )
g++ -std=c++17 -O2 -s -static -mwindows ^
    src\main.cpp app_res.o -o %OUT% ^
    -lgdiplus -lgdi32 -luser32 -lshell32 -lwinmm -lshlwapi
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    exit /b 1
)
echo Built %OUT%
for %%F in (%OUT%) do echo Size: %%~zF bytes
endlocal
