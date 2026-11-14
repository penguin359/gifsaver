@echo off
rem build-win32.cmd - build gifsaver.scr with MSVC and vcpkg's giflib.
rem
rem Needs Visual Studio with the C++ workload and a vcpkg checkout in
rem VCPKG_ROOT. giflib is installed from vcpkg.json into .\vcpkg_installed
rem as a static library, so the .scr has no DLL dependencies.
rem Extra arguments go to cl, e.g.: build-win32.cmd /DGIFSAVER_EMBED_GIF=1

setlocal
cd /d "%~dp0"
set TRIPLET=x64-windows-static

where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto fail
:have_cl

if not defined VCPKG_ROOT goto no_vcpkg
"%VCPKG_ROOT%\vcpkg.exe" install --triplet %TRIPLET% || goto fail
set "DEPS=%CD%\vcpkg_installed\%TRIPLET%"

cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS ^
   /I"%DEPS%\include" %* anim.c wav.c platform_win32.c /Fe:gifsaver.scr ^
   /link /LIBPATH:"%DEPS%\lib" gif.lib user32.lib gdi32.lib winmm.lib ^
   /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup || goto fail
exit /b 0

:no_vs
echo %~nx0: Visual Studio with the C++ workload was not found 1>&2
exit /b 1

:no_vcpkg
echo %~nx0: set VCPKG_ROOT to your vcpkg checkout 1>&2
exit /b 1

:fail
exit /b 1
