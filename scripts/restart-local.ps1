# restart-local.ps1 - stop-local.ps1, then start-local.ps1: picks up new builds and configs.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\restart-local.ps1

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'stop-local.ps1')
& (Join-Path $PSScriptRoot 'start-local.ps1')
