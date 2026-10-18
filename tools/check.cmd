@echo off
rem Runs tools/ci_check.sh with Git Bash (a plain "bash" in PowerShell or cmd may be WSL's). Usage: tools\check.cmd [base-ref]
"C:\Program Files\Git\bin\bash.exe" "%~dp0ci_check.sh" %*
