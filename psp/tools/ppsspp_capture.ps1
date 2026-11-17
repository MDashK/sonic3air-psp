# Window screenshots of a PPSSPP test run (PPSSPP doesn't write the GE's frames back to the emulated VRAM, so dumps from the game are black).
#   .\ppsspp_capture.ps1 -Seconds 49,75 [-MaxSec 90] [-OutDir <folder>]
#   -> Runs ppsspp_run.ps1 and saves "capture_<seconds>.png" of the PPSSPP window at those times after the start.
param(
	[int[]]$Seconds = @(30),
	[int]$MaxSec = 90,
	[string]$OutDir = "$PSScriptRoot\..\out\captures"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Capture {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
	[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[Win32Capture]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null

$job = Start-Job -ScriptBlock { param($script, $max) & $script -MaxSec $max } -ArgumentList "$PSScriptRoot\ppsspp_run.ps1", $MaxSec
$start = Get-Date
foreach ($s in ($Seconds | Sort-Object)) {
	while (((Get-Date) - $start).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 200 }
	$p = Get-Process -Name "PPSSPPWindows64" -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($null -eq $p) { "no PPSSPP window at $s s"; continue }
	$rect = New-Object Win32Capture+RECT
	[Win32Capture]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
	[Win32Capture]::GetWindowRect($p.MainWindowHandle, [ref]$rect) | Out-Null
	$w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
	$bmp = New-Object System.Drawing.Bitmap $w, $h
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
	$file = Join-Path $OutDir ("capture_{0:d3}.png" -f $s)
	$bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
	$g.Dispose(); $bmp.Dispose()
	"saved $file"
}
Receive-Job -Wait -AutoRemoveJob $job
