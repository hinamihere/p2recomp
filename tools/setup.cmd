@echo off
rem Runs tools/ci_setup.sh with Git Bash (a plain "bash" in PowerShell or cmd may be WSL's). Usage: tools\setup.cmd C:/p2r-agent
"C:\Program Files\Git\bin\bash.exe" "%~dp0ci_setup.sh" %*
