@echo off
rem Launches the recompiled Persona 2: Innocent Sin.
rem Expects bin\EBOOT.dec (decrypted EBOOT), extracted\ (ISO contents) and the .iso somewhere
rem below this folder (found automatically). Extra arguments are passed through.
cd /d "%~dp0"
set EXE=build\release\p2.exe
if not exist %EXE% set EXE=build\ascii\p2.exe
if not exist %EXE% (
  echo p2.exe not found - build it first, see README.md
  pause
  exit /b 1
)
%EXE% --eboot bin\EBOOT.dec --disc extracted --ms memstick %*
