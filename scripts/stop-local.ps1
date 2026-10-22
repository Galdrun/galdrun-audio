# stop-local.ps1 - Stops what start-local.ps1 started: the core, the server and the
# client of this checkout's windows-x64 releases (matched by executable path).
# Each gets Ctrl+C in its own console first, the clean stop (the server saves its
# playback state); whatever still runs after -TimeoutSeconds is killed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\stop-local.ps1 [-TimeoutSeconds 10]

param([int] $TimeoutSeconds = 10)

$ErrorActionPreference = 'Stop'
$releases = Join-Path (Split-Path $PSScriptRoot -Parent) 'releases'

# Ctrl+C reaches only the processes of one console, so a helper process detaches
# from its own, attaches to the target's and raises it there.
$sendCtrlC = @'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class ConsoleBreak {
    [DllImport("kernel32.dll")] static extern bool FreeConsole();
    [DllImport("kernel32.dll")] static extern bool AttachConsole(uint id);
    [DllImport("kernel32.dll")] static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll")] static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint group);
    public static bool Send(uint id) {
        FreeConsole();
        if (!AttachConsole(id)) return false;
        SetConsoleCtrlHandler(IntPtr.Zero, true);
        return GenerateConsoleCtrlEvent(0, 0);
    }
}
"@
exit [int](-not [ConsoleBreak]::Send(PROCESS_ID))
'@

$running = @(Get-Process galdrun-client, galdrun-server, galdrun-core -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($releases, [StringComparison]::OrdinalIgnoreCase) })
if ($running.Count -eq 0) {
    Write-Host 'not running'
    return
}

foreach ($process in $running) {
    Write-Host "stopping $($process.ProcessName) ($($process.Id))"
    $command = $sendCtrlC.Replace('PROCESS_ID', [string] $process.Id)
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    Start-Process powershell.exe -WindowStyle Hidden -Wait `
        -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded
}
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
foreach ($process in $running) {
    $left = [int][Math]::Max(0, ($deadline - (Get-Date)).TotalMilliseconds)
    if (-not $process.WaitForExit($left)) {
        Write-Host "killing $($process.ProcessName) ($($process.Id))"
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}
Write-Host 'stopped'
