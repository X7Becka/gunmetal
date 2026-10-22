# Requires -Version 5.1
<#
.SYNOPSIS
    Read the live player state (position, angles, weapon) from gundx.exe memory.

.DESCRIPTION
    The player block is an array of 8 records of 0x3510 bytes at 0x819040
    (docs/PLAYER_FIELDS.md): position at +0x00/+0x08/+0x10, angles
    (pitch, roll, yaw) at +0x18/+0x20/+0x28, all f64, and the turret/look
    pitch at +0x2E60.  The camera pitch is `p[0x18] + p[0x2E60]` (docs/CAMERA.md
    §"углы камеры"); reading only the body angle renders several degrees above
    the frame (measured on win2: +6 deg raises the correlation 0.10 -> 0.28).
    Reading it pairs an oracle frame with the exact camera, so
    tools/re/oracle_compare.py can render the same view instead of guessing.

    Read-only: OpenProcess with PROCESS_VM_READ, nothing is written.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File state.ps1
    powershell -ExecutionPolicy Bypass -File state.ps1 -Player 1 -Raw
#>
param(
    [string]$ProcessName = "gundx",
    [int]$Player = 0,
    [switch]$Raw
)

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class GmMem
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(IntPtr handle, IntPtr address,
                                                byte[] buffer, int size,
                                                out IntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
"@

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Error "process '$ProcessName' not found"; exit 1 }

$handle = [GmMem]::OpenProcess(0x0010, $false, $proc.Id)  # PROCESS_VM_READ
if ($handle -eq [IntPtr]::Zero) {
    Write-Error ("OpenProcess failed: {0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    exit 2
}

$base = 0x819040 + $Player * 0x3510
$record = New-Object byte[] 0x2E68
$read = [IntPtr]::Zero
$ok = [GmMem]::ReadProcessMemory($handle, [IntPtr]$base, $record, $record.Length, [ref]$read)
if (-not $ok) {
    Write-Error ("ReadProcessMemory failed at 0x{0:X}: {1}" -f $base, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    exit 3
}

$levelBuf = New-Object byte[] 0x50
$levelOk = [GmMem]::ReadProcessMemory($handle, [IntPtr]0x5a4868, $levelBuf, $levelBuf.Length, [ref]$read)
$level = ""
if ($levelOk) {
    $level = ([System.Text.Encoding]::ASCII.GetString($levelBuf) -split "`0")[0]
}
[void][GmMem]::CloseHandle($handle)

$d = @(0..8 | ForEach-Object { [BitConverter]::ToDouble($record, $_ * 8) })
$turret = [BitConverter]::ToDouble($record, 0x2E60)
[pscustomobject]@{
    process = $proc.ProcessName
    level = $level
    pid = $proc.Id
    base = "0x{0:X}" -f $base
    player = $Player
    position = @($d[0], $d[1], $d[2])
    angles = @{ pitch = $d[3]; roll = $d[4]; yaw = $d[5] }
    turret_pitch = $turret
    camera_pitch = $d[3] + $turret
    raw = $d
} | ConvertTo-Json -Depth 4 -Compress
