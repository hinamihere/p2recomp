@echo off
rem Launches the recompiled Persona 2: Innocent Sin.
rem Expects bin\EBOOT.dec (decrypted EBOOT), extracted\ (ISO contents) and the .iso somewhere
rem below this folder (found automatically). Extra arguments are passed through.
cd /d "%~dp0"
if not exist build\ascii\p2.exe (
  echo build\ascii\p2.exe not found - build it first, see README.md
  pause
  exit /b 1
)
build\ascii\p2.exe --eboot bin\EBOOT.dec --disc extracted --ms memstick %*
