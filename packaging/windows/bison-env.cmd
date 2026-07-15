@echo off
REM Run this file to use this extracted bison release in the current cmd.exe
REM session only -- no persistent changes are made:
REM
REM   bison-env.cmd
REM
REM For a setup that persists across new sessions, run install.ps1 once
REM instead (PowerShell; cmd.exe has no equivalent user-PATH API).

set "BISON_ROOT=%~dp0"
set "PATH=%BISON_ROOT%bin;%PATH%"
set "BISON_LIB=%BISON_ROOT%bin\bison_abi.dll"

echo bison is on PATH for this session (try: bison-cli --help)
