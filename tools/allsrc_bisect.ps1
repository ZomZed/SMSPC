# Build an all-source DOL in a worker copy with whole source folders restored to the
# ORIGINAL objects, to bisect which group of units breaks the game.
# Usage: powershell -File tools\allsrc_bisect.ps1 -Worker w2 -Name A_enemy -Prefixes Enemy/,Animal/
param([Parameter(Mandatory)][string]$Worker, [Parameter(Mandatory)][string]$Name,
      [Parameter(Mandatory)][string[]]$Prefixes, [int]$Jobs = 6)
$ErrorActionPreference = "Continue"
$env:Path = [Environment]::GetEnvironmentVariable("Path","User") + ";" + [Environment]::GetEnvironmentVariable("Path","Machine")
$main = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $main
$w = Join-Path $root "workers\$Worker"
$outDir = Join-Path $root "dolphin_test\bisect"
New-Item -ItemType Directory -Force $outDir | Out-Null
$log = Join-Path $outDir "$Name.log"

# fresh copy of main's sources/config
foreach ($d in "src","include","config","libs","tools") {
    robocopy "$main\$d" "$w\$d" /MIR /MT:8 /NFL /NDL /NJH /NJS /NP /XD __pycache__ | Out-Null
}
Copy-Item "$main\configure.py" "$w\configure.py" -Force
Push-Location $w
New-Item -ItemType Directory -Force build_allsrc | Out-Null
foreach ($d in "binutils","compilers","tools") {
    if (-not (Test-Path "build_allsrc\$d")) { robocopy "build\$d" "build_allsrc\$d" /E /NFL /NDL /NJH /NJS /NP | Out-Null }
}
# every object under the requested folders -> link original object
$objs = Select-String -Path configure.py -Pattern '"([A-Za-z0-9_]+/[^"]+\.(?:cpp|c))"' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$skip = $objs | Where-Object { $o = $_; $Prefixes | Where-Object { $o.StartsWith($_) } }
$skipFile = "config\GMSE01\link_all_skip.txt"
Add-Content -Path $skipFile -Value ("# bisect $Name`n" + ($skip -join "`n"))
"[$Name] restoring $($skip.Count) units to original: $($Prefixes -join ', ')" | Tee-Object -FilePath $log

python configure.py --version GMSE01 --build-dir build_allsrc --link-all-source 2>&1 | Out-Null
$out = python -m ninja -j $Jobs 2>&1 | Out-String
$out | Out-File -Append $log
$dol = "build_allsrc\GMSE01\mario.dol"
if ($out -match 'FAILED' -or -not (Test-Path $dol)) {
    "[$Name] BUILD FAILED - see $log" | Tee-Object -Append -FilePath $log
} else {
    Copy-Item $dol (Join-Path $outDir "$Name.dol") -Force
    Copy-Item "build_allsrc\GMSE01\mario.elf" (Join-Path $outDir "$Name.elf") -Force  # for resolving crash PCs
    "[$Name] OK -> dolphin_test\bisect\$Name.dol ($((Get-Item $dol).Length) bytes)" | Tee-Object -Append -FilePath $log
}
# put the worker back to normal
Copy-Item "$main\$skipFile" $skipFile -Force
python configure.py --version GMSE01 2>&1 | Out-Null
Pop-Location
