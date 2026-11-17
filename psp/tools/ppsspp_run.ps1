# Test run in PPSSPP (Windows): copies the game folder into PPSSPP's memory stick and runs it until the benchmark ends.
#   .\ppsspp_run.ps1 -GameDir <folder with EBOOT.PBP, data...> -Ppsspp <PPSSPPWindows64.exe> [-MaxSec 150]
param(
	[string]$GameDir = "$PSScriptRoot\..\out\PSP\GAME\Sonic3AIR",
	[string]$Ppsspp = "E:\_PSP\ppsspp_win\PPSSPPWindows64.exe",
	[int]$MaxSec = 150
)
$dst = Join-Path (Split-Path $Ppsspp) "memstick\PSP\GAME\Sonic3AIR"

# The folder itself stays (usbhostfs on WSL may keep it open): copy over it, remove only the old outputs
New-Item -ItemType Directory -Force $dst | Out-Null
foreach ($f in @("bench.log", "render.log", "allocs.txt", "meminfo.txt")) { if (Test-Path "$dst\$f") { Remove-Item -Force "$dst\$f" -Confirm:$false } }
Get-ChildItem $dst -Filter "shot_*.bmp" | Remove-Item -Force -Confirm:$false
if (Test-Path "$dst\savedata\logfile.txt") { Remove-Item -Force "$dst\savedata\logfile.txt" -Confirm:$false }
Copy-Item -Recurse -Force "$GameDir\*" $dst

$p = Start-Process -FilePath $Ppsspp -ArgumentList "`"$dst\EBOOT.PBP`"" -PassThru
$t = 0
while ($t -lt $MaxSec -and -not $p.HasExited) {
	Start-Sleep -Seconds 5; $t += 5
	if (Test-Path "$dst\bench.log") { if ((Get-Content "$dst\bench.log" -Raw) -match "SUMMARY") { break } }
	if (Test-Path "$dst\savedata\logfile.txt") { if ((Get-Content "$dst\savedata\logfile.txt" -Raw) -match "Caught unhandled|bad_alloc") { break } }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false; "stopped after $t s" } else { "exited after $t s" }
