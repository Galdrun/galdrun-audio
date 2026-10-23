# Shared by the build-*.ps1 scripts in scripts: dot-source it, do not run it.

$repositoryRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent

function Invoke-Checked([string] $What) {
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)." }
}

function Install-WingetPackage([string] $Id, [string[]] $Arguments = @()) {
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw "winget is missing; install $Id manually." }
    Write-Host "Installing $Id ..."
    & winget.exe install --exact --id $Id --accept-source-agreements --accept-package-agreements @Arguments
    if ($LASTEXITCODE -ne 0) { throw "winget could not install $Id (exit $LASTEXITCODE)." }
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

# Visual Studio 2022 Build Tools (C++) and CMake, installed with winget when missing.
function Initialize-Msvc {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vc = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vc) {
        Install-WingetPackage Microsoft.VisualStudio.2022.BuildTools @('--override', '--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    }
    if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) { Install-WingetPackage Kitware.CMake }
}

# Makes dotnet.exe the SDK the global.json in the current directory names (or a later
# feature band of it), installing it into %LOCALAPPDATA%\Microsoft\dotnet with
# dotnet-install when missing.
function Initialize-DotNet {
    $found = $false
    if (Get-Command dotnet.exe -ErrorAction SilentlyContinue) { & dotnet.exe --version *> $null; $found = $LASTEXITCODE -eq 0 }
    if (-not $found) {
        $dotnetRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\dotnet'
        $env:DOTNET_ROOT = $dotnetRoot
        $env:Path = "$dotnetRoot;$env:Path"
        if (Test-Path (Join-Path $dotnetRoot 'dotnet.exe')) { & dotnet.exe --version *> $null; $found = $LASTEXITCODE -eq 0 }
        if (-not $found) {
            $installer = Join-Path ([System.IO.Path]::GetTempPath()) 'dotnet-install.ps1'
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            (New-Object System.Net.WebClient).DownloadFile('https://dot.net/v1/dotnet-install.ps1', $installer)
            & $installer -JSonFile (Join-Path (Get-Location) 'global.json') -InstallDir $dotnetRoot
            Remove-Item -LiteralPath $installer
        }
    }
    Write-Host "Using .NET SDK $(& dotnet.exe --version)"
}

# Runs a build-*.sh script in WSL Ubuntu: first as root for missing packages, so the
# build never waits for a sudo password, then as the user. Installs Ubuntu when missing.
function Invoke-WslBuild([string] $Script, [string[]] $Targets) {
    $env:WSL_UTF8 = '1'
    $distributions = if (Get-Command wsl.exe -ErrorAction SilentlyContinue) { @(& wsl.exe --list --quiet) } else { @() }
    if ($distributions -notcontains 'Ubuntu') {
        & wsl.exe --install -d Ubuntu
        throw 'WSL Ubuntu is being installed. Restart Windows if asked, open Ubuntu once to create your user, then run this script again.'
    }
    & wsl.exe -d Ubuntu -u root --cd $repositoryRoot --exec bash $Script --packages-only @Targets
    Invoke-Checked 'Installing Ubuntu packages'
    & wsl.exe -d Ubuntu --cd $repositoryRoot --exec bash $Script @Targets
    Invoke-Checked 'Building in WSL'
}
