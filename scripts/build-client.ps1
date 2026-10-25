# build-client.ps1 - Build the Galdrun client on Windows: windows-x64 here, every
# other target through scripts/build-client.sh in WSL Ubuntu. Installs what is missing
# first; running it again skips what is already installed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\build-client.ps1                    # all targets
#   powershell -ExecutionPolicy Bypass -File scripts\build-client.ps1 -Target connector2
#   powershell -ExecutionPolicy Bypass -File scripts\build-client.ps1 -Target windows,linux-armv8
#
# Targets:
#   all (default)  everything below
#   windows        windows-x64
#   linux          linux-x86, linux-x64, linux-armv6, linux-armv7, linux-armv8, linux-armv9
#   esp32          esp32-a1s, esp32-s3-devkitc
#   connector2     Raumfeld Connector 2 firmware with the linux-armv6 client
#   rpi1 rpi3 rpi4 rpi5  Raspberry Pi SD images (ARMv6 / ARMv7 / ARM64)
#
# Installs when missing:
#   Visual Studio 2022 Build Tools (C++)  C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools, via winget
#   CMake                                 C:\Program Files\CMake, via winget
#   WSL Ubuntu                            then asks you to create your Ubuntu user and run this again
#   everything else                       inside Ubuntu, as listed in scripts/build-client.sh
# Results go to releases\client\<target>, the Connector 2 USB image to
# releases\client\raumfeld\connector2. Stops at the first failure.
param(
    [ValidateSet('all', 'windows', 'linux', 'esp32', 'connector2', 'rpi1', 'rpi3', 'rpi4', 'rpi5', 'windows-x64',
        'linux-x86', 'linux-x64', 'linux-armv6', 'linux-armv7', 'linux-armv8', 'linux-armv9',
        'esp32-a1s', 'esp32-s3-devkitc')]
    [string[]] $Target = 'all'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'setup\windows.ps1')
$windows = [bool]($Target | Where-Object { $_ -in 'all', 'windows', 'windows-x64' })
$wslTargets = @($Target | ForEach-Object {
        switch ($_) { 'all' { 'linux', 'esp32', 'connector2', 'rpi1', 'rpi3', 'rpi4', 'rpi5' } 'windows' {} 'windows-x64' {} default { $_ } }
    })

if ($windows) {
    Initialize-Msvc
    Write-Host '== windows-x64'
    $client = Join-Path $root 'client'
    & cmake -S $client -B "$client\build"
    Invoke-Checked 'Configuring windows-x64'
    & cmake --build "$client\build" --config Release --target export-client --parallel
    Invoke-Checked 'Building windows-x64'
}

if ($wslTargets) {
    Invoke-WslBuild 'scripts/build-client.sh' $wslTargets

    # The firmware build creates an SSH key for the device; keep it readable only by you.
    $key = Join-Path $root 'hardware\buildroot\private\connector2\id_ed25519'
    if (Test-Path -LiteralPath $key) {
        & icacls.exe $key /inheritance:r /grant:r "$($env:USERDOMAIN)\$($env:USERNAME):(F)" | Out-Null
        Invoke-Checked "Restricting $key"
    }
}
Write-Host "Done. Results are in $(Join-Path $root 'releases\client')"
