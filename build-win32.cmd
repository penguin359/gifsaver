@echo off
rem build-win32.cmd - build gifsaver.scr with MSVC and vcpkg's giflib.
rem
rem Needs Visual Studio with the C++ workload and a vcpkg checkout in
rem VCPKG_ROOT. giflib is installed from vcpkg.json into .\vcpkg_installed
rem as a static library, so the .scr has no DLL dependencies.
rem Extra arguments go to cl, e.g.: build-win32.cmd /DGIFSAVER_EMBED_GIF=1
rem Set DEBUG=1 for an unoptimized build with symbols (gifsaver.pdb).

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

set "OPT=/O2 /MT"
set "LIBDIR=%DEPS%\lib"
set "LINKDBG="
if not "%DEBUG%"=="1" goto opt_done
set "OPT=/Od /Zi /MTd /Fdgifsaver-cl.pdb"
set "LIBDIR=%DEPS%\debug\lib"
set "LINKDBG=/DEBUG"
:opt_done

rc /nologo /fo platform_win32.res platform_win32.rc || goto fail
cl /nologo %OPT% /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%DEPS%\include" %* anim.c wav.c platform_win32.c platform_win32.res ^
   /Fe:gifsaver.scr ^
   /link /LIBPATH:"%LIBDIR%" %LINKDBG% gif.lib user32.lib gdi32.lib winmm.lib ^
   comdlg32.lib advapi32.lib ^
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
