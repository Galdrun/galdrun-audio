# copy-server-gui.ps1 - Copy browser files into an existing server release.
# No build or server restart is needed; refresh the browser after copying.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/copy-server-gui.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/copy-server-gui.ps1 -Target linux-x64
param(
    [ValidateSet('windows-x64', 'windows-armv8', 'linux-x64', 'linux-armv7', 'linux-armv8')]
    [string] $Target = 'windows-x64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$server = Join-Path $root "releases/server/$Target"
if (-not (Test-Path -LiteralPath $server -PathType Container)) {
    throw "Server release not found: $server. Run scripts/build-server.ps1 -Target $Target first."
}

$gui = Join-Path $server 'gui'
$assets = Join-Path $gui 'assets'
New-Item -ItemType Directory -Path $assets -Force | Out-Null
Copy-Item -Path (Join-Path $root 'server-gui/www/*') -Destination $gui -Recurse -Force
foreach ($name in @('theme.css', 'i18n.js', 'shared.en.js', 'shared.de.js', 'usage-charts.js')) {
    Copy-Item -LiteralPath (Join-Path $root "shared/web/$name") -Destination $assets -Force
}
Write-Host "Copied ServerGui to $gui. Refresh the browser to load the changes."
