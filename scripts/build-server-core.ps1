# build-server-core.ps1 - Build the Galdrun core audio server on Windows: windows-x64 and
# windows-armv8 here, the Linux targets through scripts/build-server-core.sh in WSL Ubuntu. Installs what is
# missing first; running it again skips what is already installed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\build-server-core.ps1                    # all targets
#   powershell -ExecutionPolicy Bypass -File scripts\build-server-core.ps1 -Target windows,linux-armv7
#
# Targets:
#   all (default)  everything below
#   windows        windows-x64 (FFmpeg comes prebuilt from server-core\third_party\ffmpeg),
#                  windows-armv8 (cross-compiled; FFmpeg built by server-core\ffmpeg\build-windows.ps1)
#   linux          linux-x64, linux-armv7, linux-armv8, linux-armv9
#
# windows-armv8 needs the Visual Studio component "MSVC ARM64 build tools"; the script stops and
# names it when it is missing. The x64 host cannot run the result, so it is not started here.
#
# Installs when missing:
#   Visual Studio 2022 Build Tools (C++)  via winget
#   CMake                                 via winget
#   WSL Ubuntu                            then asks you to create your Ubuntu user and run this again
#   everything else                       inside Ubuntu, as listed in scripts/build-server-core.sh
# Results go to releases\server-core\<target>. Stops at the first failure.
param(
    [ValidateSet('all', 'windows', 'linux', 'windows-x64', 'windows-armv8',
        'linux-x64', 'linux-armv7', 'linux-armv8', 'linux-armv9')]
    [string[]] $Target = 'all'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'setup\windows.ps1')
$windowsTargets = @($Target | ForEach-Object {
        switch -Wildcard ($_) { 'all' { 'windows-x64', 'windows-armv8' } 'windows' { 'windows-x64', 'windows-armv8' } 'windows-*' { $_ } }
    } | Select-Object -Unique)
$wslTargets = @($Target | ForEach-Object {
        switch -Wildcard ($_) { 'all' { 'linux' } 'windows*' {} default { $_ } }
    })

if ($windowsTargets) { Initialize-Msvc }
foreach ($name in $windowsTargets) {
    Write-Host "== $name"
    if ($name -eq 'windows-armv8') {
        Assert-MsvcArm64
        $ffmpeg = Join-Path $root 'server-core\ffmpeg'
        if (-not (Test-Path (Join-Path $ffmpeg 'dist\windows-armv8-shared\bin\avformat.lib'))) {
            # The same LGPL shared build with Schannel as the vendored x64 drop.
            $env:BUILD_SHARED = '1'
            $env:EXTRA_CONFIGURE = '--enable-schannel'
            & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $ffmpeg 'build-windows.ps1') -Target armv8
            Invoke-Checked 'Building FFmpeg for windows-armv8'
        }
    }
    Push-Location (Join-Path $root 'server-core')
    try {
        & cmake --preset $name
        Invoke-Checked "Configuring $name"
        & cmake --build --preset $name --target export-server --parallel
        Invoke-Checked "Building $name"
    }
    finally { Pop-Location }
}
if ($wslTargets) { Invoke-WslBuild 'scripts/build-server-core.sh' $wslTargets }
Write-Host "Done. Results are in $(Join-Path $root 'releases\server-core')"
