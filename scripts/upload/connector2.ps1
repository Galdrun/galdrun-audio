# connector2.ps1 - Update the Galdrun client on a Raumfeld Connector 2 over SSH.
# Replaces the client only; settings, room ID and firmware stay. A new firmware is
# installed with the USB stick instead (hardware\buildroot\README.md). Linux version: connector2.sh.
# SSH is off on the device until you turn it on in its settings page (Remote login).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\upload\connector2.ps1 -IP 192.168.1.38
#   powershell -ExecutionPolicy Bypass -File scripts\upload\connector2.ps1 -IP 192.168.77.1 -IdentityFile C:\keys\connector2
#
# Parameters (a missing -IP is prompted for):
#   -IP           IPv4 address of the Connector 2 (setup Wi-Fi: 192.168.77.1).
#   -User         SSH user (default root).
#   -IdentityFile SSH private key; default hardware\buildroot\private\connector2\id_ed25519 when
#                 it exists (its .pub built in via GALDRUN_SSH_PUBKEY). Without a key, ssh asks for the password
#                 twice (copy, then install); it is never stored.
#
# What it does: copies releases\client\linux-armv6 (build it first: build-client.ps1 -Target
# linux-armv6) and connector2-remote.sh to /tmp, checks that the new binary starts there, then
# stops the client, replaces binary, libraries and licenses, keeps the old binary as
# galdrun-client.previous and starts it again.
param([string] $IP, [string] $User = 'root', [string] $IdentityFile)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $IP) { $IP = Read-Host 'Connector 2 IP address' }
$address = $null
if (-not [System.Net.IPAddress]::TryParse($IP, [ref]$address) -or $address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { throw 'Enter a valid IPv4 address.' }
if ($User -notmatch '^[a-z_][a-z0-9_-]*$') { throw 'Enter a valid Linux username.' }
$release = Join-Path $root 'releases\client\linux-armv6'
if (-not (Test-Path -LiteralPath "$release\galdrun-client")) { throw "Build linux-armv6 first: build-client.ps1 -Target linux-armv6" }
if (-not $IdentityFile) {
    $default = Join-Path $root 'hardware\buildroot\private\connector2\id_ed25519'
    if (Test-Path -LiteralPath $default) { $IdentityFile = $default }
}
$sshOptions = @('-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=10')
if ($IdentityFile) { $sshOptions += @('-i', (Resolve-Path -LiteralPath $IdentityFile).Path) }
$target = "$User@$IP"

$staging = Join-Path ([System.IO.Path]::GetTempPath()) ('galdrun-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging | Out-Null
try {
    $archive = Join-Path $staging 'galdrun-client.tar'
    & tar.exe --format ustar -cf $archive -C $release .
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the release archive.' }
    Write-Host "Copying linux-armv6 client to $target ..."
    & scp.exe @sshOptions $archive (Join-Path $PSScriptRoot 'connector2-remote.sh') "${target}:/tmp/"
    if ($LASTEXITCODE -ne 0) { throw "Copying to $target failed (exit $LASTEXITCODE)." }
    & ssh.exe @sshOptions $target 'sh /tmp/connector2-remote.sh'
    if ($LASTEXITCODE -ne 0) { throw "Updating the client on $target failed (exit $LASTEXITCODE)." }
}
finally {
    Remove-Item -LiteralPath $staging -Recurse -Force
}
