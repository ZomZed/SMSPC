# Sync a worker copy with main, rebuild, verify the DOL and take a fresh baseline.
# Usage: powershell -File tools\agent_sync.ps1 -Worker w3
# -Port (PC-port workers): also copy main's port/ tree (sources, tools, docs; not the
# build folders) into the worker and record it as the worker's port/ base
# (.sync_port_manifest + .sync_port_base/), so port/tools/merge_check.py can tell the
# worker's own port/ changes from main's newer ones and 3-way merge them.
param([Parameter(Mandatory)][string]$Worker, [int]$Jobs = 8, [switch]$Port)
$ErrorActionPreference = "Continue"
$env:Path = [Environment]::GetEnvironmentVariable("Path","User") + ";" + [Environment]::GetEnvironmentVariable("Path","Machine")
$main = Split-Path -Parent $PSScriptRoot
$w = Join-Path (Split-Path -Parent $main) "workers\$Worker"
$n = 0
foreach ($d in "src","include","config","libs","docs","tools") {
    $log = robocopy "$main\$d" "$w\$d" /MIR /MT:8 /NDL /NJH /NJS /NP /FP /XD __pycache__
    foreach ($l in $log) {
        $p = ($l -split "`t")[-1].Trim()
        if ($p -and (Test-Path $p -PathType Leaf)) {
            $dst = $p.Replace($main, $w)
            if (Test-Path $dst) { (Get-Item $dst).LastWriteTime = Get-Date; $n++ }
        }
    }
}
if ($Port) {
    $plog = robocopy "$main\port" "$w\port" /E /MT:8 /NDL /NJH /NJS /NP /FP /XD build build_hl sjis smoke smoke_ext __pycache__ build_aurora* /XF *.pdb *.exe *.obj
    foreach ($l in $plog) {
        $p = ($l -split "`t")[-1].Trim()
        if ($p -and (Test-Path $p -PathType Leaf)) {
            $dst = $p.Replace($main, $w)
            if (Test-Path $dst) { (Get-Item $dst).LastWriteTime = Get-Date; $n++ }
        }
    }
    python "$main\port\tools\merge_check.py" $Worker --record
}
Copy-Item "$main\configure.py" "$w\configure.py"
foreach ($f in "AGENTS.md","CLAUDE.md") { if (Test-Path "$main\$f") { Copy-Item "$main\$f" "$w\$f" } }
if (-not (Test-Path "$w\orig\GMSJ01\files\mario.MAP")) {
    New-Item -ItemType Directory -Force "$w\orig\GMSJ01\files" | Out-Null
    Copy-Item "$main\orig\GMSJ01\files\mario.MAP" "$w\orig\GMSJ01\files\"
}
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py -or $py -like "*WindowsApps*") { $py = "$env:LOCALAPPDATA\Python\pythoncore-3.14-64\python.exe" }
if (-not (Test-Path $py)) { throw "No usable python found; put a real python.exe on PATH" }
Push-Location $w
& $py configure.py --version GMSE01 | Out-Null
& $py -m ninja -j $Jobs 2>&1 | Out-Null
$ok = & build\tools\dtk.exe shasum -c config\GMSE01\build.sha1 2>&1
& $py -m ninja baseline 2>&1 | Out-Null
Pop-Location
Start-Sleep -Seconds 1
Set-Content -Path "$w\.sync_stamp" -Value (Get-Date -Format o)
# Content manifest of main at sync time, so agent_merge can tell real conflicts from
# files whose timestamp changed but content did not.
$manifest = foreach ($d in "src","include","config","libs") {
    Get-ChildItem "$main\$d" -Recurse -File | ForEach-Object {
        "{0}`t{1}" -f $_.FullName.Substring($main.Length + 1), (Get-FileHash $_.FullName -Algorithm MD5).Hash
    }
}
Set-Content -Path "$w\.sync_manifest" -Value $manifest -Encoding utf8
"$Worker synced ($n files refreshed): $ok"
