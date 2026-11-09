# Galdrun client updater, shipped beside galdrun-client.exe. Not meant to be
# run by hand: the client starts it after the server uploaded a package
# (client/include/app/SelfUpdate.h) and exits. It waits for that process, then
# always starts a client again with the same arguments -- the new one when the
# update worked, the current one otherwise.
#
# Only what the release archive contains is replaced; the old files go to
# .update\previous. Windows PowerShell 5.1.
param(
    [Parameter(Mandatory = $true)][ValidateSet('install', 'rollback')][string] $Action,
    [int] $ClientPid = 0,
    [string] $Sha256 = '',
    [string] $Arguments = '',
    # Base64 of the client's config path, the trial's owner (SelfUpdate.cpp handOver).
    [string] $Owner = ''
)
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$work = Join-Path $dir '.update'
$binary = Join-Path $dir 'galdrun-client.exe'
New-Item -ItemType Directory -Force $work | Out-Null

function Say([string] $message) {
    $line = '{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $message
    Add-Content -LiteralPath (Join-Path $work 'update.log') -Value $line -Encoding utf8
}

# Puts the previous files back over the ones the last install placed.
function Restore {
    $installed = Join-Path $work 'installed'
    $previous = Join-Path $work 'previous'
    if (-not (Test-Path -LiteralPath $installed)) { return $false }
    foreach ($name in Get-Content -LiteralPath $installed) {
        if ($name) { Remove-Item -LiteralPath (Join-Path $dir $name) -Recurse -Force -ErrorAction SilentlyContinue }
    }
    if (Test-Path -LiteralPath $previous) {
        Get-ChildItem -LiteralPath $previous -Force | Move-Item -Destination $dir
    }
    Remove-Item -LiteralPath $previous, $installed -Recurse -Force -ErrorAction SilentlyContinue
    return $true
}

function Install-Package {
    $package = Join-Path $work 'package.zip'
    $staged = Join-Path $work 'staged'
    if (-not (Test-Path -LiteralPath $package)) { throw 'no package' }
    $actual = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) { throw "checksum mismatch: $actual, expected $Sha256" }
    Remove-Item -LiteralPath $staged -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $package -DestinationPath $staged
    $source = $staged
    # A release archive holds one folder, galdrun-client-<version>-<target>.
    if (-not (Test-Path -LiteralPath (Join-Path $source 'galdrun-client.exe'))) {
        $source = (Get-ChildItem -LiteralPath $staged -Directory | Select-Object -First 1).FullName
    }
    $newBinary = Join-Path $source 'galdrun-client.exe'
    if (-not $source -or -not (Test-Path -LiteralPath $newBinary)) { throw 'the package has no galdrun-client.exe' }
    # Refuse a binary that does not start here before touching the installed one.
    $version = & $newBinary --version 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "the new client does not start: $version" }

    $previous = Join-Path $work 'previous'
    $installed = Join-Path $work 'installed'
    Remove-Item -LiteralPath $previous, $installed -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $previous | Out-Null
    New-Item -ItemType File $installed | Out-Null
    try {
        foreach ($item in Get-ChildItem -LiteralPath $source -Force) {
            $target = Join-Path $dir $item.Name
            if (Test-Path -LiteralPath $target) { Move-Item -LiteralPath $target -Destination $previous }
            Add-Content -LiteralPath $installed -Value $item.Name -Encoding utf8
            Move-Item -LiteralPath $item.FullName -Destination $dir
        }
    } catch {
        Restore | Out-Null
        throw
    }
    # "pending <owner>", the owner's bytes as the client passed them (no BOM).
    $trial = [Text.Encoding]::ASCII.GetBytes('pending ') + [Convert]::FromBase64String($Owner) + [byte[]](10)
    [IO.File]::WriteAllBytes((Join-Path $work 'trial'), [byte[]]$trial)
    Say "installed $($version.Trim()); on trial"
}

# The old client has to be gone: its files are locked while it runs.
if ($ClientPid -gt 0) {
    try { Wait-Process -Id $ClientPid -Timeout 60 -ErrorAction Stop } catch [System.TimeoutException] {
        Say "the client (PID $ClientPid) did not exit; not updating"
        exit 1
    } catch { }
}

try {
    if ($Action -eq 'install') {
        Install-Package
    } elseif (Restore) {
        Say 'rolled back to the previous client'
    } else {
        Say 'nothing to roll back'
    }
} catch {
    Say "update failed; the current client keeps running: $($_.Exception.Message)"
} finally {
    Remove-Item -LiteralPath (Join-Path $work 'staged'), (Join-Path $work 'package.zip') -Recurse -Force -ErrorAction SilentlyContinue
    if ($Action -eq 'rollback') { Remove-Item -LiteralPath (Join-Path $work 'trial') -Force -ErrorAction SilentlyContinue }
}

$clientArguments = if ($Arguments) { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Arguments)) } else { '' }
$start = @{ FilePath = $binary; WorkingDirectory = (Get-Location).Path }
if ($clientArguments) { $start.ArgumentList = $clientArguments }
Start-Process @start
