# Windows build helper for the audio-only FFmpeg build.
#
# Sets up a portable MSYS2/UCRT64 toolchain inside tools\msys64
# (downloaded once, reused afterwards) and runs build.sh inside a UCRT64
# shell. Nothing outside this folder is created or modified.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build-windows.ps1
#
# build.sh environment variables (BUILD_SHARED, EXTRA_DECODERS, ...) can be
# set in the calling shell; they are passed through, e.g.:
#   $env:BUILD_SHARED = '1'; .\build-windows.ps1
param()

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$tools = Join-Path $root 'tools'
$msys  = Join-Path $tools 'msys64'
New-Item -ItemType Directory -Force -Path $tools | Out-Null

function Header($msg) { Write-Host "==> $msg" -ForegroundColor Green }

# --- 1. portable MSYS2 (downloaded once) ------------------------------------
$fresh = $false
if (-not (Test-Path (Join-Path $msys 'usr\bin\bash.exe'))) {
    $fresh = $true
    Header 'fetching latest MSYS2 base archive ...'
    $rel   = Invoke-RestMethod 'https://api.github.com/repos/msys2/msys2-installer/releases/latest'
    $asset = $rel.assets | Where-Object { $_.name -match '^msys2-base-x86_64-.*\.tar\.xz$' } | Select-Object -First 1
    if (-not $asset) { throw 'msys2-base tar.xz asset not found in latest release' }
    $tarball = Join-Path $tools $asset.name
    Header "downloading $($asset.name) ($([math]::Round($asset.size/1MB,1)) MB) ..."
    & curl.exe -fL --retry 3 --progress-bar -o $tarball $asset.browser_download_url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $($asset.browser_download_url)" }
    Header 'extracting MSYS2 (thousands of files, antivirus slows this down) ...'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & tar -xJf $tarball -C $tools
    if ($LASTEXITCODE -ne 0) { throw 'extraction failed' }
    Header "extraction done in $([int]$sw.Elapsed.TotalSeconds)s"
    Remove-Item $tarball
}

$bash = Join-Path $msys 'usr\bin\bash.exe'
function Msys-Run([string]$cmd) {
    & $bash -lc $cmd
    if ($LASTEXITCODE -ne 0) { throw "msys2 command failed: $cmd" }
}

# --- 2. toolchain via pacman (first run downloads ~300 MB and installs
#        ~100k files; antivirus scanning makes this slow - it is one-time) --
if ($fresh) {
    Header 'initializing package keyring (one-time, may take a few minutes) ...'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Msys-Run 'pacman-key --init >/dev/null 2>&1; pacman-key --populate msys2 >/dev/null 2>&1; true'
    Header "keyring done in $([int]$sw.Elapsed.TotalSeconds)s"
}
Header 'ensuring UCRT64 toolchain is installed (one-time, ~300 MB download) ...'
$sw = [Diagnostics.Stopwatch]::StartNew()
Msys-Run 'pacman -Sy --needed --noconfirm --disable-download-timeout make nasm tar xz curl diffutils coreutils mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-crt'
Header "toolchain ready in $([int]$sw.Elapsed.TotalSeconds)s"

# --- 3. run build.sh inside the UCRT64 environment ---------------------------
Header 'running build.sh in UCRT64 shell ...'
$msysRoot = ('/' + $root.Substring(0, 1).ToLower() + $root.Substring(2)).Replace('\', '/')
$env:MSYSTEM = 'UCRT64'
& $bash -lc "cd '$msysRoot' && ./build.sh"
exit $LASTEXITCODE
