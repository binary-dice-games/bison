<#
.SYNOPSIS
  Persistently adds this extracted bison release's bin\ folder to your user
  PATH, so `bison-cli` and bison_abi.dll are discoverable from any new
  terminal session -- no administrator rights needed (this edits the
  per-user HKCU environment, not the machine-wide one).

.DESCRIPTION
  Run once:
    powershell -ExecutionPolicy Bypass -File .\install.ps1
  Then open a *new* terminal (PATH changes don't apply to already-open
  ones) and run: bison-cli --help

  For a one-off session instead of a persistent change, dot-source
  bison-env.ps1 instead and skip this script entirely.

.PARAMETER Uninstall
  Removes this release's bin\ folder from your user PATH again.
#>
param([switch]$Uninstall)

$BisonRoot = $PSScriptRoot
$BinDir = Join-Path $BisonRoot "bin"
$CurrentPath = [Environment]::GetEnvironmentVariable("Path", "User")
$Entries = @($CurrentPath -split ';' | Where-Object { $_ -ne "" })

if ($Uninstall) {
    $NewEntries = $Entries | Where-Object { $_ -ne $BinDir }
    [Environment]::SetEnvironmentVariable("Path", ($NewEntries -join ';'), "User")
    Write-Host "Removed $BinDir from your user PATH."
    Write-Host "Open a new terminal for the change to take effect."
} elseif ($Entries -contains $BinDir) {
    Write-Host "$BinDir is already on your user PATH -- nothing to do."
} else {
    $NewPath = ($Entries + $BinDir) -join ';'
    [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    Write-Host "Added $BinDir to your user PATH."
    Write-Host "Open a new terminal for it to take effect (try: bison-cli --help)"
    Write-Host "To undo: powershell -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall"
}
