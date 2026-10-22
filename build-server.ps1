# build-server.ps1 - Build the Galdrun server (.NET) for the selected targets, installing
# the .NET SDK first when it is missing. Running it again skips what is already there.
# .NET publishes every target from any host, so this also builds the Linux targets; no WSL.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build-server.ps1                    # all targets
#   powershell -ExecutionPolicy Bypass -File build-server.ps1 -Target windows-x64,linux-armv7
#
# Targets (all self-contained: nothing has to be installed on the device):
#   all (default)  windows and linux
#   windows        windows-x64, windows-armv8
#   linux          linux-x64, linux-armv7, linux-armv8
#
# Installs when missing:
#   .NET SDK  the version in server\global.json, into %LOCALAPPDATA%\Microsoft\dotnet (dotnet-install)
# Results go to releases\server\<target>. Stops at the first failure.
param(
    [ValidateSet('all', 'windows', 'linux', 'windows-x64', 'windows-armv8', 'linux-x64', 'linux-armv7', 'linux-armv8')]
    [string[]] $Target = 'all'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\setup\windows.ps1')
$rids = @{ 'windows-x64' = 'win-x64'; 'windows-armv8' = 'win-arm64'; 'linux-x64' = 'linux-x64'; 'linux-armv7' = 'linux-arm'; 'linux-armv8' = 'linux-arm64' }
$selected = @($Target | ForEach-Object {
        switch ($_) {
            'all' { 'windows-x64', 'windows-armv8', 'linux-x64', 'linux-armv7', 'linux-armv8' }
            'windows' { 'windows-x64', 'windows-armv8' }
            'linux' { 'linux-x64', 'linux-armv7', 'linux-armv8' }
            default { $_ }
        }
    } | Select-Object -Unique)

$server = Join-Path $PSScriptRoot 'server'
Push-Location $server
try {
    Initialize-DotNet

    foreach ($name in $selected) {
        Write-Host "== $name"
        & dotnet.exe publish 'src\Multiroom.Server\Multiroom.Server.csproj' -c Release -r $rids[$name] `
            --self-contained true -o (Join-Path $PSScriptRoot "releases\server\$name")
        Invoke-Checked "Publishing $name"
    }
}
finally { Pop-Location }
Write-Host "Done. Results are in $(Join-Path $PSScriptRoot 'releases\server')"
