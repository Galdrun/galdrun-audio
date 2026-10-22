# build-mobile-gui.ps1 - Build the Galdrun app's desktop version for the selected targets,
# installing the .NET SDK first when it is missing. .NET publishes every target from any
# host, so this also builds the Linux targets; no WSL. Early work: Android needs the .NET
# android workload and iOS a Mac, so neither is built here.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build-mobile-gui.ps1                    # all targets
#   powershell -ExecutionPolicy Bypass -File build-mobile-gui.ps1 -Target windows-x64
#
# Targets (all self-contained: nothing has to be installed on the device):
#   all (default)  windows and linux
#   windows        windows-x64, windows-armv8
#   linux          linux-x64, linux-armv7, linux-armv8
#
# Installs when missing:
#   .NET SDK  the version in mobile-gui\global.json, into %LOCALAPPDATA%\Microsoft\dotnet (dotnet-install)
# Results go to releases\mobile-gui\<target>. Stops at the first failure.
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

Push-Location (Join-Path $PSScriptRoot 'mobile-gui')
try {
    Initialize-DotNet
    foreach ($name in $selected) {
        Write-Host "== $name"
        & dotnet.exe publish 'src\Multiroom.App.Desktop\Multiroom.App.Desktop.csproj' -c Release -r $rids[$name] `
            --self-contained true -nodeReuse:false -o (Join-Path $PSScriptRoot "releases\mobile-gui\$name")
        Invoke-Checked "Publishing $name"
    }
}
finally { Pop-Location }
Write-Host "Done. Results are in $(Join-Path $PSScriptRoot 'releases\mobile-gui')"
