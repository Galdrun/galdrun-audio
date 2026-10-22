# build-server-core.ps1 - Build the Galdrun core audio server on Windows: windows-x64
# here, the Linux targets through build-server-core.sh in WSL Ubuntu. Installs what is
# missing first; running it again skips what is already installed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build-server-core.ps1                    # all targets
#   powershell -ExecutionPolicy Bypass -File build-server-core.ps1 -Target windows,linux-armv7
#
# Targets:
#   all (default)  everything below
#   windows        windows-x64 (FFmpeg comes prebuilt from server-core\third_party\ffmpeg)
#   linux          linux-x86, linux-x64, linux-armv7, linux-armv8, linux-armv9
#
# Installs when missing:
#   Visual Studio 2022 Build Tools (C++)  via winget
#   CMake                                 via winget
#   WSL Ubuntu                            then asks you to create your Ubuntu user and run this again
#   everything else                       inside Ubuntu, as listed in build-server-core.sh
# Results go to releases\server-core\<target>. Stops at the first failure.
param(
    [ValidateSet('all', 'windows', 'linux', 'windows-x64',
        'linux-x86', 'linux-x64', 'linux-armv7', 'linux-armv8', 'linux-armv9')]
    [string[]] $Target = 'all'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\setup\windows.ps1')
$windows = [bool]($Target | Where-Object { $_ -in 'all', 'windows', 'windows-x64' })
$wslTargets = @($Target | ForEach-Object {
        switch ($_) { 'all' { 'linux' } 'windows' {} 'windows-x64' {} default { $_ } }
    })

if ($windows) {
    Initialize-Msvc
    Write-Host '== windows-x64'
    Push-Location (Join-Path $PSScriptRoot 'server-core')
    try {
        & cmake --preset windows-x64
        Invoke-Checked 'Configuring windows-x64'
        & cmake --build --preset windows-x64 --target export-server --parallel
        Invoke-Checked 'Building windows-x64'
    }
    finally { Pop-Location }
}
if ($wslTargets) { Invoke-WslBuild 'build-server-core.sh' $wslTargets }
Write-Host "Done. Results are in $(Join-Path $PSScriptRoot 'releases\server-core')"
