@echo off
rem Removes Vocahansments from VOCALOID4 (your themes in %APPDATA%\Vocahansments are kept).
setlocal EnableDelayedExpansion
rem Same folder prompt and sanitising as install.cmd.
set "ARG=%~1"
set "TRIES=0"
if defined ARG goto check
:ask
set /a TRIES+=1
if !TRIES! gtr 5 (echo No valid folder entered. & pause & exit /b 1)
set "ARG="
set /p "ARG=Enter the VOCALOID4 folder (the one with VOCALOID4.exe, or drag it here): "
if not defined ARG goto ask
:check
set "V4=!ARG:"=!"
if not defined V4 goto ask
if "!V4:~-1!"=="\" set "V4=!V4:~0,-1!"
if not exist "!V4!\VOCALOID4.exe" (
  echo VOCALOID4.exe not found in "!V4!".
  goto ask
)
rem Same elevation handoff as install.cmd: run cmd.exe /c ""script" "path""
rem explicitly, since the .cmd RunAs verb strips the quotes.
fltmc >nul 2>&1 && goto elevated
set "SELF=%~f0"
powershell -NoProfile -Command "try { Start-Process -Verb RunAs -FilePath cmd.exe -ArgumentList ('/c ' + [char]34 + [char]34 + $env:SELF + [char]34 + ' ' + [char]34 + $env:V4 + [char]34 + [char]34) -ErrorAction Stop } catch { exit 1 }"
if errorlevel 1 (echo Administrator rights are needed to uninstall from "%V4%". & pause & exit /b 1)
exit /b 0
:elevated
tasklist /FI "IMAGENAME eq VOCALOID4.exe" | "%SystemRoot%\System32\find.exe" /I "VOCALOID4.exe" >nul && (echo Close VOCALOID4 first. & pause & exit /b 1)
if exist "%V4%\version.dll" del /F /Q "%V4%\version.dll"
if exist "%V4%\vhmods" rmdir /S /Q "%V4%\vhmods"
if exist "%V4%\version.dll" goto fail
if exist "%V4%\vhmods" goto fail
echo Vocahansments removed from "%V4%".
pause & exit /b 0
:fail
echo Some files could not be removed from "%V4%". & pause & exit /b 1
