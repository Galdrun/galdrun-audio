# build-server.ps1 - Build the Galdrun server (.NET) for the selected targets, installing
# the .NET SDK first when it is missing. Running it again skips what is already there.
# .NET publishes every target from any host, so this also builds the Linux targets; no WSL.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\build-server.ps1                    # all targets
#   powershell -ExecutionPolicy Bypass -File scripts\build-server.ps1 -Target windows-x64,linux-armv7
#   powershell -ExecutionPolicy Bypass -File scripts\build-server.ps1 -Target rpi4   # Pi server SD image
#
# Targets (all self-contained: nothing has to be installed on the device):
#   all (default)  windows and linux
#   windows        windows-x64, windows-armv8
#   linux          linux-x64, linux-armv7, linux-armv8, linux-armv9
#   raspberrypi    rpi3, rpi4, rpi5: SD images with server, core and client (Pi 3 or newer),
#                  to releases\server\raspberrypi\<board>. Each also builds its core
#                  (scripts\build-server-core.ps1) and its client image (scripts\build-client.ps1,
#                  WSL); the image itself is assembled in WSL. Not part of all.
#
# linux-armv9 is the linux-arm64 build under its own name (.NET has no ARMv9 RID).
#
# Installs when missing:
#   .NET SDK  the version in server\global.json, into %LOCALAPPDATA%\Microsoft\dotnet (dotnet-install)
# Results go to releases\server\<target>. Stops at the first failure.
param(
    [ValidateSet('all', 'windows', 'linux', 'windows-x64', 'windows-armv8', 'linux-x64', 'linux-armv7', 'linux-armv8', 'linux-armv9',
        'raspberrypi', 'rpi3', 'rpi4', 'rpi5')]
    [string[]] $Target = 'all'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'setup\windows.ps1')
$rids = @{ 'windows-x64' = 'win-x64'; 'windows-armv8' = 'win-arm64'; 'linux-x64' = 'linux-x64'; 'linux-armv7' = 'linux-arm'; 'linux-armv8' = 'linux-arm64'; 'linux-armv9' = 'linux-arm64' }
# The server, core and client architecture of each Pi image (hardware/buildroot/board/rpi*).
$boards = [ordered]@{ 'rpi3' = 'linux-armv7'; 'rpi4' = 'linux-armv8'; 'rpi5' = 'linux-armv8' }
$images = @($Target | ForEach-Object { if ($_ -eq 'raspberrypi') { $boards.Keys } elseif ($boards.Contains($_)) { $_ } } |
    Select-Object -Unique)
$selected = @(@($Target | Where-Object { $_ -ne 'raspberrypi' -and -not $boards.Contains($_) }) + @($images | ForEach-Object { $boards[$_] }) | ForEach-Object {
        switch ($_) {
            'all' { 'windows-x64', 'windows-armv8', 'linux-x64', 'linux-armv7', 'linux-armv8', 'linux-armv9' }
            'windows' { 'windows-x64', 'windows-armv8' }
            'linux' { 'linux-x64', 'linux-armv7', 'linux-armv8', 'linux-armv9' }
            default { $_ }
        }
    } | Select-Object -Unique)

$server = Join-Path $root 'server'
Push-Location $server
try {
    Initialize-DotNet

    foreach ($name in $selected) {
        Write-Host "== $name"
        & dotnet.exe publish 'src\Multiroom.Server\Multiroom.Server.csproj' -c Release -r $rids[$name] `
            --self-contained true -o (Join-Path $root "releases\server\$name")
        Invoke-Checked "Publishing $name"
    }
}
finally { Pop-Location }
foreach ($board in $images) {
    Write-Host "== $board"
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-server-core.ps1') -Target $boards[$board]
    Invoke-Checked "Building the $($boards[$board]) core"
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-client.ps1') -Target $board
    Invoke-Checked "Building the $board client image"
    $env:WSL_UTF8 = '1'
    & wsl.exe -d Ubuntu --cd $root --exec bash hardware/buildroot/scripts/build-server-image.sh $board "releases/server/raspberrypi/$board"
    Invoke-Checked "Assembling the $board server image"
}
Write-Host "Done. Results are in $(Join-Path $root 'releases\server')"
