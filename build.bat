@echo off
setlocal

rem ---- Xbox 360 FPS counter build script ----
rem Usage: build.bat          (builds hello\hello.cpp into out\fps_hello.xex)
rem        build.bat overlay  (builds overlay\overlay.cpp into out\fps_hello.xex)

set "SRC=hello\hello.cpp"
set "EXTRA_OBJ="
if /i "%~1"=="overlay" set "SRC=overlay\overlay.cpp"
if /i "%~1"=="overlay" set "EXTRA_OBJ=out\fps_core.obj"

if "%XEDK%"=="" set "XEDK=C:\Program Files (x86)\Microsoft Xbox 360 SDK"

if not exist "%XEDK%\bin\win32\cl.exe" (
    echo ERROR: cannot find "%XEDK%\bin\win32\cl.exe"
    echo Check that the Xbox 360 SDK folder above is correct.
    exit /b 1
)

set "PATH=%XEDK%\bin\win32;%PATH%"
set "INCLUDE=%XEDK%\include\xbox;%XEDK%\include\win32"
set "LIB=%XEDK%\lib\xbox"

if not exist out mkdir out

echo === compile ===
cl.exe /nologo /c /TP /O2 /GS- /Gy /W3 /D_XBOX /DNDEBUG %SRC% /Foout\hello.obj
if errorlevel 1 goto :fail
if defined EXTRA_OBJ (
    cl.exe /nologo /c /TC /O2 /GS- /Gy /W3 /D_XBOX /DNDEBUG /D_CRT_SECURE_NO_WARNINGS core\fps_core.c /Foout\fps_core.obj
    if errorlevel 1 goto :fail
)

echo === link ===
link.exe /nologo /DLL /BASE:0x8B000000 /OUT:out\fps_hello.dll out\hello.obj %EXTRA_OBJ% libcmt.lib xapilib.lib xboxkrnl.lib
if errorlevel 1 goto :fail

echo === make xex ===
imagexex.exe /nologo /OUT:out\fps_hello.xex out\fps_hello.dll
if errorlevel 1 goto :fail

echo === convert to retail ===
if exist xextool.exe (
    .\xextool.exe -m r -r a out\fps_hello.xex
    if errorlevel 1 goto :fail
) else (
    echo WARNING: xextool.exe not found, xex is NOT retail converted
)

echo.
echo SUCCESS: out\fps_hello.xex
exit /b 0

:fail
echo.
echo BUILD FAILED. Copy everything above this line and paste it to Claude.
exit /b 1
