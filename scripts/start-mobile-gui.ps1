# start-mobile-gui.ps1 - Starts the Galdrun app's desktop version from the windows-x64 release,
# in a window the size of a common phone screen (390 x 844); -Desktop opens it at its desktop size.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\start-mobile-gui.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\start-mobile-gui.ps1 -Desktop
#
# Build it first: scripts\build-mobile-gui.ps1 -Target windows-x64.

param([switch] $Desktop)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$path = Join-Path $root 'releases\mobile-gui\windows-x64\galdrun.exe'
if (-not (Test-Path $path)) { throw "$path not found - build it first" }
$start = @{ FilePath = $path; WorkingDirectory = (Split-Path $path) }
if (-not $Desktop) { $start.ArgumentList = '--phone' }
Start-Process @start | Out-Null
