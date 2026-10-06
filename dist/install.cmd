@echo off
rem Vocahansments installer - copies the mod loader and mods into VOCALOID4.
setlocal EnableDelayedExpansion
rem The folder comes from the argument (the elevated relaunch passes it) or
rem is asked for until it holds VOCALOID4.exe (5 tries, so a closed stdin
rem can't spin forever).
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
rem Drop any stray quotes (never valid in a Windows path) and a trailing
rem backslash, so a fumbled entry can't break the checks below.
set "V4=!ARG:"=!"
if not defined V4 goto ask
if "!V4:~-1!"=="\" set "V4=!V4:~0,-1!"
if not exist "!V4!\VOCALOID4.exe" (
  echo VOCALOID4.exe not found in "!V4!".
  goto ask
)
rem Relaunch elevated as cmd.exe /c ""script" "path"". Running the .cmd itself
rem with RunAs goes through cmd /C "%1" %*, which strips the quotes, and
rem Start-Process adds none. Values travel as env vars so cmd never parses them.
fltmc >nul 2>&1 && goto elevated
set "SELF=%~f0"
powershell -NoProfile -Command "try { Start-Process -Verb RunAs -FilePath cmd.exe -ArgumentList ('/c ' + [char]34 + [char]34 + $env:SELF + [char]34 + ' ' + [char]34 + $env:V4 + [char]34 + [char]34) -ErrorAction Stop } catch { exit 1 }"
if errorlevel 1 (echo Administrator rights are needed to install into "%V4%". & pause & exit /b 1)
exit /b 0
:elevated
tasklist /FI "IMAGENAME eq VOCALOID4.exe" | "%SystemRoot%\System32\find.exe" /I "VOCALOID4.exe" >nul && (echo Close VOCALOID4 first. & pause & exit /b 1)
copy /Y "%~dp0version.dll" "%V4%\version.dll" >nul || goto fail
xcopy /E /I /Y "%~dp0vhmods" "%V4%\vhmods" >nul || goto fail
echo Vocahansments installed into "%V4%".
echo Start VOCALOID4 - a "Mods" menu and a knife button appear.
pause & exit /b 0
:fail
echo Copy failed. & pause & exit /b 1
