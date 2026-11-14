@echo off
rem Builds dshrink.exe with MSVC x64, no C runtime. Works from any prompt.
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || (echo Visual Studio C++ tools not found & exit /b 1)

:build
if not exist obj mkdir obj
rem /GS- /Gs: no CRT security cookie or stack probes (stacks are committed up
rem front instead: /STACK here, and the worker thread's CreateThread size).
cl /nologo /O1 /GS- /Gs9999999 /W4 /utf-8 /Foobj\ /c src\dshrink.c || exit /b 1
rc /nologo /fo obj\dshrink.res src\dshrink.rc || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
     /STACK:1048576,1048576 /MANIFEST:NO /EMITTOOLVERSIONINFO:NO /OUT:dshrink.exe ^
     obj\dshrink.obj obj\dshrink.res kernel32.lib user32.lib gdi32.lib comctl32.lib ^
     shell32.lib comdlg32.lib advapi32.lib ole32.lib uuid.lib wininet.lib bcrypt.lib || exit /b 1
for %%f in (dshrink.exe) do echo dshrink.exe: %%~zf bytes
