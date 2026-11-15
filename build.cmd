@echo off
rem Builds GameConsole.dll - the single plain-C replacement for the old
rem .NET Native AOT build plus the native helper DLLs. Run from any prompt;
rem locates Visual Studio itself.
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio C++ tools not found.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist GameConsole.c (
  echo GameConsole.c not found next to this script.
  exit /b 1
)
cl /nologo /LD /O2 /W3 GameConsole.c /Fe:GameConsole.dll
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
del GameConsole.obj GameConsole.exp GameConsole.lib 2>nul
echo GameConsole.dll built OK.
