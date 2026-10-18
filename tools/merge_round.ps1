# Merge a decomp worker into main, verify everything, re-sync the worker; prints a short summary.
# Usage: powershell -File tools\merge_round.ps1 -Worker w1 [-NoSync]
#   1. agent_merge (copies the worker's changed src/include files; stops on conflicts)
#   2. full `python -m ninja` (the DOL check after ALL header edits: a header can break a linked unit)
#   3. the PC port build (ship.ps1 -NoSmoke)
#   4. agent_sync of the worker (unless -NoSync), so it is ready for the next batch
param([Parameter(Mandatory)][string]$Worker, [switch]$NoSync)
$ErrorActionPreference = "Continue"
$main = Split-Path -Parent $PSScriptRoot
Set-Location $main
$py = "$env:LOCALAPPDATA\Python\pythoncore-3.14-64\python.exe"
if (-not (Test-Path $py)) { $py = "python" }
$m = & "$main\tools\agent_merge.ps1" -Worker $Worker -NoBaseline 2>&1 | ForEach-Object { "$_" }
$conf = $m | Select-String "CONFLICT"
$files = ($m | Select-String "  merge").Count
"merge: $files file(s)" + $(if ($conf) { "; CONFLICTS:`n" + ($conf -join "`n") } else { "" })
$n = & $py -m ninja 2>&1 | ForEach-Object { "$_" }
$dol = $n | Select-String "mario.dol: (OK|FAILED)" | Select-Object -Last 1
"dol:   " + $(if ($dol) { $dol.Line.Trim() } else { "build/GMSE01/mario.dol: OK (no relink needed)" })
$n | Select-String "  All:|  Game Code:" | ForEach-Object { "prog: " + $_.Line.Trim() }
if ($dol -and $dol.Line -match "FAILED") { "STOP: the DOL no longer matches; fix before syncing."; exit 1 }
$s = & "$main\port\tools\ship.ps1" -NoSmoke 2>&1 | ForEach-Object { "$_" }
"port:  " + (($s | Select-String "compiled:|port build failed" | Select-Object -Last 1).Line)
if (-not $NoSync -and -not $conf) {
    $y = & "$main\tools\agent_sync.ps1" -Worker $Worker 2>&1 | ForEach-Object { "$_" }
    "sync:  " + (($y | Select-String "synced" | Select-Object -Last 1).Line)
}
