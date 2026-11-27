@echo off
setlocal

rem ---- PC-side checks (no console needed) ----
rem Builds and runs the core unit tests, then renders overlay previews with fps.ini
rem into out\pc\preview_*.png. Uses Visual Studio 2010's x86 compiler.
rem Usage: test.bat

set "VC=C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat"
if not exist "%VC%" (
    echo ERROR: cannot find "%VC%"
    exit /b 1
)
call "%VC%" x86 >nul
if not exist out\pc mkdir out\pc

echo === unit tests ===
cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /Icore /Feout\pc\test_core.exe /Foout\pc\ tests\test_core.c core\fps_core.c >nul
if errorlevel 1 goto :fail
out\pc\test_core.exe
if errorlevel 1 goto :fail

echo === previews ===
cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /Icore /Feout\pc\preview.exe /Foout\pc\ tools\preview.c core\fps_core.c >nul
if errorlevel 1 goto :fail
for %%S in (bright dark busy) do (
    out\pc\preview.exe out\pc\preview_%%S.ppm %%S 30 fps.ini enabled=1
    python -I tools\ppm2png.py out\pc\preview_%%S.ppm out\pc\preview_%%S.png 0 0 400 260
)
out\pc\preview.exe out\pc\preview_all.ppm dark 30 fps.ini show_stats=1 show_graph=1 show_temps=1
python -I tools\ppm2png.py out\pc\preview_all.ppm out\pc\preview_all.png 0 0 400 260

echo.
echo SUCCESS: tests passed, previews in out\pc\preview_*.png
exit /b 0

:fail
echo.
echo FAILED. Copy everything above this line and paste it to Claude.
exit /b 1
