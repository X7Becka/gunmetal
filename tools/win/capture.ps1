# Requires -Version 5.1
<#
.SYNOPSIS
    Capture the Gunmetal window (the "oracle": original game under DXWnd +
    dgVoodoo) into a PNG.

.DESCRIPTION
    Runs on Windows, not in WSL. The game must already be running (start it
    through DXWnd, windowed). The script finds the process' main window,
    brings it to the foreground, waits, and saves the window rectangle.

    With -State the script also reads the live player state right after the
    frame and writes the sidecar JSON next to it (docs/ORACLE.md).  The block
    is sampled twice - just before and just after CopyFromScreen - so a frame
    taken while the camera was still moving is flagged (state_stable=false):
    a sidecar read by a second process, seconds later, does not reproduce the
    frame (measured on win2: the frame is pitched ~6 deg below the recorded
    state).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File capture.ps1 -ProcessName gundx -Out level0.png

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File capture.ps1 -State `
        -GameDir 'D:\games\Gunmetal 3dfx' `
        -Out '\\wsl.localhost\Ubuntu\home\nooverload\gunmetal-re\out\oracle\win4.png'
#>
param(
    [string]$ProcessName = "gundx",
    [string]$Out = "frame.png",
    [int]$WaitMs = 1500,
    [ValidateSet("client", "window")]
    [string]$Mode = "client",
    [switch]$State,
    [string]$Sidecar = "",
    [int]$Player = 0,
    [string]$GameDir = "",
    [double]$EyeHeight = 10.0,
    [int]$RenderWidth = 320,
    [int]$RenderHeight = 200
)

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class OracleWin32
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }

    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
}
"@

if ($State) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class OracleMem
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

    # The player block: 8 records of 0x3510 at 0x819040, then the level name
    # buffer at 0x5a4868 (docs/PLAYER_FIELDS.md, docs/LEVEL_LOAD.md S17).
    function Read-PlayerState {
        param([IntPtr]$Handle, [int]$Slot)
        $record = New-Object byte[] 0x30
        $read = [IntPtr]::Zero
        $addr = [IntPtr](0x819040 + $Slot * 0x3510)
        if (-not [OracleMem]::ReadProcessMemory($Handle, $addr, $record,
                                                $record.Length, [ref]$read)) {
            return $null
        }
        $levelBuf = New-Object byte[] 0x50
        $level = ""
        if ([OracleMem]::ReadProcessMemory($Handle, [IntPtr]0x5a4868, $levelBuf,
                                           $levelBuf.Length, [ref]$read)) {
            $level = ([System.Text.Encoding]::ASCII.GetString($levelBuf) -split "`0")[0]
        }
        $d = @(0..5 | ForEach-Object { [BitConverter]::ToDouble($record, $_ * 8) })
        return @{
            level = $level
            pos = @($d[0], $d[1], $d[2])
            pitch = $d[3]
            roll = $d[4]
            yaw = $d[5]
        }
    }

    # SHA-256 of the level pair, read from the installation (never written).
    function Get-LevelHash {
        param([string]$Dir, [string]$Level)
        if (-not $Dir -or -not $Level) { return $null }
        $sbt = Join-Path $Dir ($Level.ToLower() + ".sbt")
        $sbw = Join-Path $Dir ($Level.ToLower() + ".sbw")
        $out = @{}
        foreach ($pair in @(@("sbt", $sbt), @("sbw", $sbw))) {
            if (Test-Path $pair[1]) {
                $out[$pair[0]] = (Get-FileHash -Algorithm SHA256 -Path $pair[1]).Hash.ToLower()
            }
        }
        return $out
    }
}

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } |
        Select-Object -First 1

if (-not $proc) {
    Write-Error "process '$ProcessName' with a visible window not found; start the game first"
    exit 1
}

$hwnd = $proc.MainWindowHandle
[void][OracleWin32]::ShowWindow($hwnd, 9)   # SW_RESTORE
[void][OracleWin32]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds $WaitMs

$win = New-Object OracleWin32+RECT
if (-not [OracleWin32]::GetWindowRect($hwnd, [ref]$win)) {
    Write-Error "GetWindowRect failed"
    exit 2
}

if ($Mode -eq "client") {
    $client = New-Object OracleWin32+RECT
    if (-not [OracleWin32]::GetClientRect($hwnd, [ref]$client)) {
        Write-Error "GetClientRect failed"
        exit 2
    }
    $origin = New-Object OracleWin32+POINT
    if (-not [OracleWin32]::ClientToScreen($hwnd, [ref]$origin)) {
        Write-Error "ClientToScreen failed"
        exit 2
    }
    $left = $origin.X
    $top = $origin.Y
    $width = $client.Right - $client.Left
    $height = $client.Bottom - $client.Top
} else {
    $left = $win.Left
    $top = $win.Top
    $width = $win.Right - $win.Left
    $height = $win.Bottom - $win.Top
}

# A maximised game window can be larger than the display: capture the visible
# part only, otherwise CopyFromScreen returns black for the off-screen rest.
Add-Type -AssemblyName System.Windows.Forms
$screenRect = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$x0 = [Math]::Max($left, $screenRect.Left)
$y0 = [Math]::Max($top, $screenRect.Top)
$x1 = [Math]::Min($left + $width, $screenRect.Right)
$y1 = [Math]::Min($top + $height, $screenRect.Bottom)
if ($x1 -le $x0 -or $y1 -le $y0) {
    Write-Error "window is outside the screen"
    exit 3
}

$bmp = New-Object System.Drawing.Bitmap ($x1 - $x0), ($y1 - $y0)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)

$memHandle = [IntPtr]::Zero
$before = $null
$after = $null
if ($State) {
    $memHandle = [OracleMem]::OpenProcess(0x0010, $false, $proc.Id)  # VM_READ
    if ($memHandle -eq [IntPtr]::Zero) {
        Write-Error ("OpenProcess failed: {0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        exit 4
    }
    $before = Read-PlayerState -Handle $memHandle -Slot $Player
}

$gfx.CopyFromScreen($x0, $y0, 0, 0, $bmp.Size)

if ($State) {
    $after = Read-PlayerState -Handle $memHandle -Slot $Player
    [void][OracleMem]::CloseHandle($memHandle)
}
$gfx.Dispose()

$full = [System.IO.Path]::GetFullPath($Out)
$dir = [System.IO.Path]::GetDirectoryName($full)
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
# Write through a stream: GDI+ Save() is unreliable on \\wsl.localhost paths.
$ms = New-Object System.IO.MemoryStream
$bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
[System.IO.File]::WriteAllBytes($full, $ms.ToArray())
$ms.Dispose()

[pscustomobject]@{
    out = $full
    process = $proc.ProcessName
    mode = $Mode
    window = @{ x = $win.Left; y = $win.Top
                w = $win.Right - $win.Left; h = $win.Bottom - $win.Top }
    captured = @{ x = $x0; y = $y0; w = $x1 - $x0; h = $y1 - $y0 }
    screen = @{ x = $screenRect.X; y = $screenRect.Y
                w = $screenRect.Width; h = $screenRect.Height }
} | ConvertTo-Json -Compress

if (-not $State) { exit 0 }

if (-not $after) {
    Write-Error "state read failed; no sidecar written"
    exit 5
}

# Stability: the two samples straddle CopyFromScreen; any drift means the
# camera moved between the frame and the read, which is exactly the failure
# measured on win2 (frame pitched ~6 deg below the recorded state).
$stable = $true
if ($before) {
    $dp = 0.0
    for ($k = 0; $k -lt 3; $k++) {
        $dp += [Math]::Abs($before.pos[$k] - $after.pos[$k])
    }
    $da = [Math]::Abs($before.pitch - $after.pitch) +
          [Math]::Abs($before.roll - $after.roll) +
          [Math]::Abs($before.yaw - $after.yaw)
    $stable = ($dp -le 0.5) -and ($da -le 1.0e-3)
}

$cam = @{
    pos = @($after.pos[0], $after.pos[1], $after.pos[2] + $EyeHeight)
    player_pos = @($after.pos[0], $after.pos[1], $after.pos[2])
    yaw = $after.yaw
    pitch = $after.pitch
    roll = $after.roll
    eye_height = $EyeHeight
}
$hashes = Get-LevelHash -Dir $GameDir -Level $after.level
$side = [ordered]@{
    level = $after.level
    sbt_sha256 = $(if ($hashes) { $hashes["sbt"] } else { $null })
    sbw_sha256 = $(if ($hashes) { $hashes["sbw"] } else { $null })
    save = $null
    camera = $cam
    render = @{ width = $RenderWidth; height = $RenderHeight
                api = "dgVoodoo"; scale = 1 }
    capture = @{ tool = "tools/win/capture.ps1 -State"; mode = $Mode
                 screen = @($screenRect.Width, $screenRect.Height)
                 rect = @($x0, $y0, $x1 - $x0, $y1 - $y0)
                 file = [System.IO.Path]::GetFileName($full) }
    state_source = "tools/win/capture.ps1 -State (same process, straddling the capture)"
    state_stable = $stable
    state_before = $before
    notes = $(if ($stable) { "state sampled around the frame" } else { "camera moved during the capture: do not use for pixel comparison" })
}

if (-not $Sidecar) {
    $Sidecar = [System.IO.Path]::ChangeExtension($full, ".json")
}
$json = ($side | ConvertTo-Json -Depth 6)
$utf8 = New-Object System.Text.UTF8Encoding ($false)
[System.IO.File]::WriteAllText($Sidecar, $json, $utf8)

[pscustomobject]@{
    sidecar = $Sidecar
    level = $after.level
    stable = $stable
    pos = $cam.pos
    yaw = $after.yaw
    pitch = $after.pitch
} | ConvertTo-Json -Compress

