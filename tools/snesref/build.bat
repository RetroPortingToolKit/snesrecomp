@echo off
REM Build snesref.exe from this directory.
REM
REM Prerequisites you must supply locally (intentionally NOT committed):
REM   - SDL2 dev package extracted here as SDL2-2.30.9\  (https://libsdl.org),
REM     or point SDL2_DIR at an extracted SDL2 VC dev package elsewhere.
REM   - at runtime, a libretro SNES core DLL (e.g. snes9x_libretro.dll),
REM     passed as argv[1]. The core is licensed separately from this tool.
REM Optional: OUT_DIR = directory for snesref.exe (default: this directory).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d %~dp0
if not defined SDL2_DIR set "SDL2_DIR=SDL2-2.30.9"
if not defined OUT_DIR set "OUT_DIR=."
cl /nologo /EHsc /O2 /MD /W3 /D_CRT_SECURE_NO_WARNINGS frontend.cpp /I "%SDL2_DIR%\include" /Fo:"%OUT_DIR%\\" /Fe:"%OUT_DIR%\snesref.exe" /link /SUBSYSTEM:CONSOLE "%SDL2_DIR%\lib\x64\SDL2.lib" shell32.lib user32.lib
echo BUILD_EXIT=%ERRORLEVEL%
