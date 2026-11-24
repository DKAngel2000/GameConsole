@echo off
rem Copies the built single-DLL GameConsole into the game folder.
rem
rem There is only ONE file to deploy: GameConsole.dll (built by build.cmd,
rem ~250 KB, plain C). The game's artifact.json must name it in load_dlls:
rem
rem     "load_dlls": ["gameconsole.dll", "dse.dll"]
rem
rem This DLL self-starts from DllMain (a game loader only calls LoadLibraryW).
setlocal
set "GAME=H:\Games\FINAL FANTASY RESONANCE\FFRS\Binaries\Win64"
set "SRC=%~dp0"

if not exist "%GAME%" (
  echo Game folder not found: %GAME%
  echo Edit the GAME= line at the top of this script.
  exit /b 1
)
if not exist "%SRC%GameConsole.c" (
  echo GameConsole.c not found next to this script.
  exit /b 1
)
if not exist "%SRC%GameConsole.dll" (
  echo GameConsole.dll not found. Run:  build.cmd
  exit /b 1
)

rem warn if the source is newer than the built DLL
for %%A in ("%SRC%GameConsole.c")   do set "SRCT=%%~tA"
for %%A in ("%SRC%GameConsole.dll") do set "DLLT=%%~tA"
if "%SRCT%" GTR "%DLLT%" echo WARNING: GameConsole.c is newer than GameConsole.dll - run build.cmd first.

for %%F in ("%SRC%GameConsole.dll") do if %%~zF LSS 100000 (
  echo WARNING: GameConsole.dll is only %%~zF bytes - that does not look like the built DLL. Run build.cmd.
  exit /b 1
)

copy /y "%SRC%GameConsole.dll" "%GAME%\GameConsole.dll" >nul
if errorlevel 1 (
  echo Copy failed. Is the game running / is the folder writable?
  exit /b 1
)

for %%F in ("%GAME%\GameConsole.dll") do set "SIZE=%%~zF"
echo Deployed to %GAME%
echo   GameConsole.dll %SIZE% bytes (only file needed)
echo.
echo artifact.json must contain:  "load_dlls": ["gameconsole.dll", "dse.dll"]
echo First [INFO] line at startup should read:  hooks: IAT capture on [artifact.dll] ...
