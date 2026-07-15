# Dot-source this file to use this extracted bison release in the current
# PowerShell session only -- no persistent changes are made:
#
#   . .\bison-env.ps1
#
# For a setup that persists across new sessions, run .\install.ps1 once
# instead.
#
# Windows' DLL search order includes every directory on PATH, so putting
# bin\ on PATH is enough for bison_abi.dll to be found both by `bison-cli`
# itself and by a separate program (e.g. a C# app P/Invoking into it, or
# Python's ctypes) -- unlike Linux, no separate "library path" variable is
# needed. BISON_LIB is set anyway since bindings/python/bison reads it
# directly and skips its own search entirely when present.

$BisonRoot = $PSScriptRoot
$env:PATH = "$BisonRoot\bin;$env:PATH"
$env:BISON_LIB = "$BisonRoot\bin\bison_abi.dll"

Write-Host "bison is on PATH for this session (try: bison-cli --help)"
