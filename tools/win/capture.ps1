# Requires -Version 5.1
<#
.SYNOPSIS
    Capture the Gunmetal window (the "oracle": original game under DXWnd +
    dgVoodoo) into a PNG.

.DESCRIPTION
    Runs on Windows, not in WSL. The game must already be running (start it
    through DXWnd, windowed). The script finds the process' main window,
    brings it to the foreground, waits, and saves the window rectangle.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File capture.ps1 -ProcessName gundx -Out level0.png
#>
param(
    [string]$ProcessName = "gundx",
    [string]$Out = "frame.png",
    [int]$WaitMs = 1500,
    [ValidateSet("client", "window")]
    [string]$Mode = "client"
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
$gfx.CopyFromScreen($x0, $y0, 0, 0, $bmp.Size)
$gfx.Dispose()

$full = [System.IO.Path]::GetFullPath($Out)
$dir = [System.IO.Path]::GetDirectoryName($full)
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

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
