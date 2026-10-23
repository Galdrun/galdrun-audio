# esp32.ps1 - Flash an ESP32 client release to a board over USB serial (esptool).
# The ESP32 has no SSH or network update; this needs the USB cable. Linux version: esp32.sh.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\upload\esp32.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\upload\esp32.ps1 -Port COM5 -Baud 115200
#   powershell -ExecutionPolicy Bypass -File scripts\upload\esp32.ps1 -Board esp32-s3-devkitc -Port COM7
#
# Parameters:
#   -Port   serial port of the board; may be left out when exactly one is present.
#   -Board  board name under releases\client (default esp32-a1s); must be built first.
#   -Baud   flash baud rate (default 460800; drop to 115200 when a cable is unreliable).
#
# What it does: checks the port and the release images, downloads Espressif's standalone
# esptool once (checksum-verified, into %LOCALAPPDATA%\galdrun), probes the chip first
# (flash_id) so a wrong port or dead wiring fails before anything is written, flashes
# bootloader, partition table and app at the offsets in flash_args, then hard-resets the
# board. The storage partition is not flashed; the client formats it on first start.
# Status goes to scripts\.run\flash-client-esp32-<port>.log.
param(
    [string]$Port,
    [string]$Board = 'esp32-a1s',
    [int]$Baud = 460800
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$release = Join-Path $root "releases\client\$Board"
if (-not (Test-Path -LiteralPath (Join-Path $release 'flash_args'))) { throw "No release for '$Board' under releases\client - build it first: scripts\build-client.ps1 -Target $Board" }
if ($Baud -notin 9600..921600) { throw 'Baud must be between 9600 and 921600.' }
$ports = @([System.IO.Ports.SerialPort]::GetPortNames())
if (-not $Port -and $ports.Count -eq 1) { $Port = $ports[0] }
if ($ports -notcontains $Port) { throw "Give -Port. Available: $($ports -join ', ')" }
$chip = (Get-Content -LiteralPath (Join-Path $release 'flasher_args.json') -Encoding UTF8 | ConvertFrom-Json).extra_esptool_args.chip
if (-not $chip) { throw 'flasher_args.json names no chip; flash this release manually with esptool.' }

# Standalone esptool, no Python needed. v4 matches the flash_args ESP-IDF 5.5 writes.
$esptoolVersion = 'v4.12.0'
$esptoolDir = Join-Path $env:LOCALAPPDATA "galdrun\esptool-$esptoolVersion"
$esptool = Join-Path $esptoolDir 'esptool-windows-amd64\esptool.exe'
if (-not (Test-Path -LiteralPath $esptool)) {
    $zip = Join-Path ([System.IO.Path]::GetTempPath()) "esptool-$esptoolVersion-windows-amd64.zip"
    Write-Host "Downloading esptool $esptoolVersion ..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    (New-Object System.Net.WebClient).DownloadFile("https://github.com/espressif/esptool/releases/download/$esptoolVersion/esptool-$esptoolVersion-windows-amd64.zip", $zip)
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash -ne '42FDDC5E6A05716868AD77FB43ACBF53BE041F97ABED87FF850DF1DC88140889') {
        Remove-Item -LiteralPath $zip
        throw 'Downloaded esptool does not match its expected SHA256.'
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $esptoolDir -Force
    Remove-Item -LiteralPath $zip
}

$run = Join-Path $root 'scripts\.run'
New-Item -ItemType Directory -Force -Path $run | Out-Null
$status = Join-Path $run "flash-client-esp32-$Port.log"
Set-Content -LiteralPath $status -Value "Flashing $Board ($chip) on $Port"
try {
    Write-Host "Probing $chip on $Port ..."
    & $esptool --chip $chip -p $Port --before default_reset --after hard_reset flash_id
    if ($LASTEXITCODE -ne 0) { throw "No $chip answered on $Port - check port, cable and USB serial driver." }
    Write-Host "Flashing $Board to $Port at $Baud baud ..."
    Push-Location $release
    try {
        & $esptool --chip $chip -p $Port -b $Baud --before default_reset --after hard_reset write_flash '@flash_args'
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($code -ne 0) { throw "Flashing failed (exit $code)." }
    Set-Content -LiteralPath $status -Value "Success: $Board ($chip) flashed on $Port"
    Write-Host "Done. The board was reset and now runs the client; see client\esp32\README.md for Wi-Fi setup."
}
catch {
    Set-Content -LiteralPath $status -Value "Failed: $($_.Exception.Message)"
    throw
}
