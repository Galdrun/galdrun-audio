# Windows build helper for the audio-only FFmpeg build.
#
# Sets up a portable MSYS2/UCRT64 toolchain inside tools\msys64
# (downloaded once, reused afterwards) and runs build.sh inside a UCRT64
# shell. Nothing outside this folder is created or modified.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build-windows.ps1                # x64
#   powershell -ExecutionPolicy Bypass -File .\build-windows.ps1 -Target armv8  # ARM64
#
# armv8 cross-compiles from the x64 host with llvm-mingw (aarch64-w64-mingw32),
# downloaded once into tools\llvm-mingw. The x64 host cannot run the result, so
# build.sh skips its run-time checks for it.
#
# build.sh environment variables (BUILD_SHARED, EXTRA_DECODERS, ...) can be
# set in the calling shell; they are passed through, e.g.:
#   $env:BUILD_SHARED = '1'; .\build-windows.ps1
param([ValidateSet('x64', 'armv8')] [string] $Target = 'x64')

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

function Msys-Path([string]$path) { ('/' + $path.Substring(0, 1).ToLower() + $path.Substring(2)).Replace('\', '/') }

# --- 3. ARM64 only: llvm-mingw cross toolchain (downloaded once) -------------
$prefix = ''
if ($Target -eq 'armv8') {
    $llvm = Join-Path $tools 'llvm-mingw'
    if (-not (Test-Path (Join-Path $llvm 'bin\aarch64-w64-mingw32-clang.exe'))) {
        Header 'fetching latest llvm-mingw release ...'
        $rel   = Invoke-RestMethod 'https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest'
        $asset = $rel.assets | Where-Object { $_.name -match '^llvm-mingw-.*-ucrt-x86_64\.zip$' } | Select-Object -First 1
        if (-not $asset) { throw 'llvm-mingw ucrt-x86_64 zip asset not found in latest release' }
        $zip = Join-Path $tools $asset.name
        Header "downloading $($asset.name) ($([math]::Round($asset.size/1MB,1)) MB) ..."
        & curl.exe -fL --retry 3 --progress-bar -o $zip $asset.browser_download_url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $($asset.browser_download_url)" }
        Header 'extracting llvm-mingw ...'
        & tar -xf $zip -C $tools
        if ($LASTEXITCODE -ne 0) { throw 'extraction failed' }
        Remove-Item $zip
        Move-Item (Join-Path $tools ($asset.name -replace '\.zip$', '')) $llvm
    }
    $env:TARGET_ARCH = 'armv8'
    $env:NO_RUN_CHECKS = '1'
    $env:EXTRA_CONFIGURE = ('--enable-cross-compile --arch=aarch64 --target-os=mingw32 ' +
        "--cross-prefix=aarch64-w64-mingw32- --cc=aarch64-w64-mingw32-clang $env:EXTRA_CONFIGURE").Trim()
    # The login shell rebuilds PATH, so the toolchain is prepended inside it.
    $prefix = "export PATH='$(Msys-Path (Join-Path $llvm 'bin'))':`$PATH && "
}

# --- 4. run build.sh inside the UCRT64 environment ---------------------------
Header "running build.sh ($Target) in UCRT64 shell ..."
$env:MSYSTEM = 'UCRT64'
& $bash -lc "cd '$(Msys-Path $root)' && $prefix./build.sh"
exit $LASTEXITCODE
