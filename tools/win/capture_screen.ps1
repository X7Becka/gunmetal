# Requires -Version 5.1
<#
.SYNOPSIS
    Capture the desktop (fullscreen game) into a PNG, with the display geometry.

.DESCRIPTION
    The game runs without DXWnd in dgVoodoo fullscreen mode: the 320x200 image
    is stretched with aspect kept (ScalingMode = stretched_ar), so it appears
    centred with black pillars.  This script captures the primary screen and
    prints the geometry as JSON; tools/re/oracle_compare.py --auto-crop then
    finds the actual picture rectangle.

    Prefer windowed capture (capture.ps1 + DXWnd) when pixel-exact frames are
    needed: fullscreen capture is upscaled by the display and edges are softer.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File capture_screen.ps1 -Out level0.png
    powershell -ExecutionPolicy Bypass -File capture_screen.ps1 -Out l0.png -Region 96,0,1728,1080
#>
param(
    [string]$Out = "screen.png",
    [string]$Region = ""
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$rect = New-Object System.Drawing.Rectangle($bounds.X, $bounds.Y, $bounds.Width, $bounds.Height)
if ($Region -ne "") {
    $p = $Region.Split(",") | ForEach-Object { [int]$_.Trim() }
    if ($p.Count -ne 4) { Write-Error "Region must be x,y,w,h"; exit 2 }
    $rect = New-Object System.Drawing.Rectangle($p[0], $p[1], $p[2], $p[3])
}

$bmp = New-Object System.Drawing.Bitmap($rect.Width, $rect.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.X, $rect.Y, 0, 0, $rect.Size)
$g.Dispose()

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$full = [System.IO.Path]::GetFullPath($Out)
$ms = New-Object System.IO.MemoryStream
$bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
[System.IO.File]::WriteAllBytes($full, $ms.ToArray())
$ms.Dispose()

[pscustomobject]@{
    out = $full
    screen = @{ x = $bounds.X; y = $bounds.Y; w = $bounds.Width; h = $bounds.Height }
    captured = @{ x = $rect.X; y = $rect.Y; w = $rect.Width; h = $rect.Height }
    dpi = (Get-ItemProperty "HKCU:\Control Panel\Desktop" -Name LogPixels -ErrorAction SilentlyContinue).LogPixels
} | ConvertTo-Json -Compress
