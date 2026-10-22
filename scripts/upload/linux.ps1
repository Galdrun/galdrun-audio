# linux.ps1 - Upload and start the matching Linux client release on a Raspberry Pi
# (DietPi/Dropbear) or any Linux reachable over SSH. Linux version: linux.sh.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\upload\linux.ps1 -IP 192.168.1.31 -User dietpi
#   powershell -ExecutionPolicy Bypass -File scripts\upload\linux.ps1 -IP 192.168.1.31 -User dietpi -Server 192.168.1.20:4711
#   powershell -ExecutionPolicy Bypass -File scripts\upload\linux.ps1 -IP 192.168.1.31 -User dietpi -IdentityFile $env:USERPROFILE\.ssh\id_ed25519
#
# Parameters (missing -IP/-User are prompted for):
#   -IP           IPv4 address of the device.
#   -User         SSH username; needs sudo (DietPi's dietpi/root have passwordless sudo) for the
#                 boot service and missing ALSA packages.
#   -Server       Optional server address (host:port) the client connects to. Without it the
#                 client keeps the server in its client.json, or a server attaches it after
#                 finding its discovery beacon.
#   -IdentityFile SSH private key for non-interactive logins. Without it, OpenSSH prompts for
#                 the password on each SSH connection (2-3 times); it is never stored.
#                 One-time key setup: ssh-keygen -t ed25519, then append id_ed25519.pub to
#                 ~/.ssh/authorized_keys on the device.
#
# What it does: detects the remote architecture (ARMv6/7/8/9, x86/x64), streams the matching
# releases\client\<platform> folder over SSH stdin (no scp/sftp needed on the device), extracts
# to ~/galdrun-client, installs ALSA when missing, and installs galdrun-client.service (enabled
# at boot, restarted after crashes) using linux-remote.sh. Existing client.json and audio device
# cache are preserved. Windows OpenSSH >= 9.5 prepends a UTF-8 BOM to piped stdin; the remote
# script strips it before extracting. Status goes to scripts\.run\upload-client-<IP>.log; on the
# device check /run/galdrun-client/client.log and journalctl -u galdrun-client.
param([string] $IP, [string] $User, [string] $Server, [string] $IdentityFile)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $IP) { $IP = Read-Host 'Device IP address' }
if (-not $User) { $User = Read-Host 'SSH username' }
$address = $null
if (-not [System.Net.IPAddress]::TryParse($IP, [ref]$address) -or $address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { throw 'Enter a valid IPv4 address.' }
if ($User -notmatch '^[a-z_][a-z0-9_-]*$') { throw 'Enter a valid Linux username.' }
if ($Server -and ($Server -notmatch '^[a-zA-Z0-9][a-zA-Z0-9.-]*:[0-9]{1,5}$' -or [int]($Server.Split(':')[-1]) -notin 1..65535)) { throw 'Server must be hostname:port or IPv4:port.' }
$target = "$User@$IP"
$ssh = (Get-Command ssh.exe -ErrorAction Stop).Source
$tar = (Get-Command tar.exe -ErrorAction Stop).Source
$sshOptions = @('-o', 'StrictHostKeyChecking=accept-new', '-o', 'ConnectTimeout=10')
if ($IdentityFile) {
    $IdentityFile = (Resolve-Path -LiteralPath $IdentityFile -ErrorAction Stop).Path
    $sshOptions += @('-i', $IdentityFile)
}
$run = Join-Path $root 'scripts\.run'
New-Item -ItemType Directory -Force -Path $run | Out-Null
$status = Join-Path $run "upload-client-$IP.log"
Set-Content -LiteralPath $status -Value "Checking $target"
$archive = Join-Path ([System.IO.Path]::GetTempPath()) ("galdrun-client-" + [guid]::NewGuid().ToString('N') + '.tar.gz')
$remoteScript = '.' + [System.IO.Path]::GetFileName($archive).Replace('.tar.gz', '.sh')
$process = $null
try {
    Write-Host 'SSH will prompt for your password when needed; it is not stored.'
    $hardware = @(& $ssh @sshOptions $target 'uname -m; getconf LONG_BIT')
    if ($LASTEXITCODE -ne 0 -or $hardware.Count -ne 2) { throw 'Could not detect the remote architecture.' }
    $machine = $hardware[0].Trim()
    $bits = $hardware[1].Trim()
    switch -Regex ($machine) {
        '^aarch64$' { if ($bits -eq '64') { $platform = 'linux-armv8' } else { $platform = 'linux-armv7' }; break }
        '^armv6' { $platform = 'linux-armv6'; break }
        '^armv7|^armv8l' { $platform = 'linux-armv7'; break }
        '^x86_64$' { if ($bits -eq '64') { $platform = 'linux-x64' } else { $platform = 'linux-x86' }; break }
        '^i[3-6]86$' { $platform = 'linux-x86'; break }
        default { throw "Unsupported remote architecture: $machine ($bits-bit)" }
    }
    $release = Join-Path $root "releases\client\$platform"
    if (-not (Test-Path -LiteralPath "$release\galdrun-client")) { throw "Build $platform first: $release" }
    Write-Host "Uploading $platform to $target"
    Set-Content -LiteralPath $status -Value "Uploading $platform to $target"
    & $tar --format ustar -czf $archive -C $release .
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the release archive.' }
    $serverArgs = if ($Server) { "--server $Server " } else { '' }
    $remote = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'linux-remote.sh'))
    $remote = $remote.Replace('@SERVER_ARGS@', $serverArgs).Replace('@USER@', $User).Replace("`r`n", "`n")
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remote))
    # The script travels in the command; stdin carries the binary archive unchanged.
    $command = 'umask 077; printf %s ' + $encoded + ' | base64 -d > ' + $remoteScript + '; bash ' + $remoteScript + ' 2>&1; result=$?; rm -f ' + $remoteScript + '; exit $result'
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $ssh
    # Every variable here is validated IPv4/username, base64 or a generated filename.
    $info.Arguments = '-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 ' + $target + ' ' + $command
    if ($IdentityFile) { $info.Arguments = '-i "' + $IdentityFile + '" ' + $info.Arguments }
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $inputFile = [System.IO.File]::OpenRead($archive)
    try { $inputFile.CopyTo($process.StandardInput.BaseStream) }
    finally { $inputFile.Dispose(); $process.StandardInput.Close() }
    $result = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    Write-Host $result
    if ($process.ExitCode -ne 0) { throw 'Remote client upload/startup failed; see the SSH output.' }
    if ($result.Contains('SERVICE_SUDO_REQUIRED')) {
        Write-Host 'Installing boot service: enter the SSH/sudo password when prompted.'
        & $ssh @sshOptions -tt $target 'sudo bash "$HOME/galdrun-client/install-service.sh"'
        if ($LASTEXITCODE -ne 0) { throw 'Client boot service installation failed.' }
        $result = 'Client service installed, enabled at boot and running.'
    }
    Set-Content -LiteralPath $status -Value ("Success: $platform on $target" + [Environment]::NewLine + $result)
    Write-Host "Deployment complete. Status: $status"
}
catch {
    Set-Content -LiteralPath $status -Value ("Failed: $($_.Exception.Message)" + [Environment]::NewLine + $result)
    throw
}
finally {
    if ($process) {
        if (-not $process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
}
