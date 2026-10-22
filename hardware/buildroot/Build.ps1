[CmdletBinding()]
param(
    [string[]]$Boards = @('connector2'),
    [ValidateRange(1, 32)][int]$Jobs = 16,
    [switch]$List
)
$ErrorActionPreference = 'Stop'
foreach ($board in $Boards) {
    if ($board -notmatch '^[a-z0-9][a-z0-9_-]*$') { throw "Invalid board: $board" }
}
$linuxScript = (& wsl.exe --exec wslpath -a -u (Join-Path $PSScriptRoot 'build.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'WSL could not resolve the build script path.' }
if ($List) {
    & wsl.exe --exec bash $linuxScript --list
} else {
    & wsl.exe --exec env "JOBS=$Jobs" bash $linuxScript @Boards
}
if ($LASTEXITCODE -ne 0) { throw "Firmware build failed (exit $LASTEXITCODE). See hardware\buildroot\logs." }
if (-not $List) {
    foreach ($board in $Boards) {
        $key = Join-Path $PSScriptRoot "private\$board\id_ed25519"
        if (Test-Path -LiteralPath $key) {
            & icacls.exe $key /inheritance:r /grant:r "$($env:USERDOMAIN)\$($env:USERNAME):(F)" | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Could not restrict SSH private-key permissions: $key" }
        }
    }
}
