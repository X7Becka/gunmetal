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
    [int]$WaitMs = 1500
)

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class OracleWin32
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
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

$rect = New-Object OracleWin32+RECT
if (-not [OracleWin32]::GetWindowRect($hwnd, [ref]$rect)) {
    Write-Error "GetWindowRect failed"
    exit 2
}

$width  = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) {
    Write-Error "bad window rect: ${width}x${height}"
    exit 3
}

$bmp = New-Object System.Drawing.Bitmap $width, $height
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
$full = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out))
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose()
$bmp.Dispose()

Write-Output ("captured {0}x{1} from '{2}' -> {3}" -f $width, $height, $proc.ProcessName, $Out)
