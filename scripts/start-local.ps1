# start-local.ps1 - Starts a local Galdrun system on this PC from the windows-x64 releases:
# the core, the server and one client, each in its own console window (Ctrl+C there stops it).
# Configs are in scripts\.run\config, data and logs in scripts\.run; both stay out of git.
# A missing config is created with local defaults.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\start-local.ps1
#
# Build the three first: build-server-core.ps1, build-server.ps1, build-client.ps1 -Target windows-x64.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$run = Join-Path $PSScriptRoot '.run'
$config = Join-Path $run 'config'

$running = @(Get-Process galdrun-core, galdrun-server, galdrun-client -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) })
if ($running.Count -gt 0) { throw "already running: $(($running | ForEach-Object ProcessName) -join ', ')" }

$defaults = @{
    'server.json' = '{ "dataDirectory": "../server", "gui": { "logs": [ { "name": "core", "path": "../core/core.log" } ] } }'
    'core.json'   = '{}'
    # 4720 is the server's web interface, so the client's settings page moves to 4723.
    'client.json' = '{ "server": "127.0.0.1:4711", "audioAccessMode": "shared", "controlPort": 4723 }'
}
foreach ($dir in 'config', 'server', 'core', 'client') { New-Item -ItemType Directory -Force (Join-Path $run $dir) | Out-Null }
foreach ($name in $defaults.Keys) {
    $path = Join-Path $config $name
    if (-not (Test-Path $path)) {
        Set-Content -Path $path -Value $defaults[$name] -Encoding ascii
        Write-Host "created $path"
    }
}

function Start-Part([string] $name, [string] $exe, [string[]] $arguments) {
    $path = Join-Path $root "releases\$exe"
    if (-not (Test-Path $path)) { throw "$name`: $path not found - build it first" }
    Write-Host "starting $name"
    Start-Process -FilePath $path -ArgumentList $arguments -WorkingDirectory $run | Out-Null
}

Start-Part 'core' 'server-core\windows-x64\galdrun-core.exe' @('--config', "`"$config\core.json`"", '--log-file', "`"$run\core\core.log`"")
# The server connects to the core's control port right away.
Start-Sleep -Milliseconds 500
Start-Part 'server' 'server\windows-x64\galdrun-server.exe' @('--config', "`"$config\server.json`"")
Start-Part 'client' 'client\windows-x64\galdrun-client.exe' @('--config', "`"$config\client.json`"", '--audio-cache', "`"$run\client\audio_devices.json`"")
Write-Host 'Web interface: http://127.0.0.1:4720/'
