# Merge a worker's changes into main, rebuild, verify the DOL and report.
# Picks files the worker changed after its last agent_sync (.sync_stamp). A file that main
# also changed since then is reported as CONFLICT and skipped (merge it by hand).
# Usage: powershell -File tools\agent_merge.ps1 -Worker w3 [-DryRun] [-NoBaseline]
param([Parameter(Mandatory)][string]$Worker, [switch]$DryRun, [switch]$NoBaseline, [int]$Jobs = 16)
$ErrorActionPreference = "Continue"
$env:Path = [Environment]::GetEnvironmentVariable("Path","User") + ";" + [Environment]::GetEnvironmentVariable("Path","Machine")
$main = Split-Path -Parent $PSScriptRoot
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py -or $py -like "*WindowsApps*") { $py = "$env:LOCALAPPDATA/Python/pythoncore-3.14-64/python.exe" }
$w = Join-Path (Split-Path -Parent $main) "workers\$Worker"
if (-not (Test-Path "$w\.sync_stamp")) { throw "$Worker has no .sync_stamp; run agent_sync first" }
$stamp = [datetime]::Parse((Get-Content "$w\.sync_stamp" -Raw).Trim())

$manifest = @{}
if (Test-Path "$w\.sync_manifest") {
    Get-Content "$w\.sync_manifest" | ForEach-Object { $p = $_ -split "`t"; if ($p.Count -eq 2) { $manifest[$p[0]] = $p[1] } }
}
function MainChanged($rel, $m) {
    if (-not (Test-Path $m)) { return $false }
    if ($manifest.Count) {
        if (-not $manifest.ContainsKey($rel)) { return $true }   # created in main after sync
        return (Get-FileHash $m -Algorithm MD5).Hash -ne $manifest[$rel]
    }
    return (Get-Item $m).LastWriteTime -gt $stamp                # old workers: timestamp fallback
}
$picked = @(); $conflicts = @()
foreach ($d in "src","include","config","libs") {
    Get-ChildItem "$w\$d" -Recurse -File | Where-Object { $_.LastWriteTime -gt $stamp } | ForEach-Object {
        $rel = $_.FullName.Substring($w.Length + 1)
        $m = Join-Path $main $rel
        if ((Test-Path $m) -and (Get-FileHash $m).Hash -eq (Get-FileHash $_.FullName).Hash) { return }
        $bytes = [IO.File]::ReadAllBytes($_.FullName)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB) { Write-Warning "BOM in $rel" }
        if (MainChanged $rel $m) { $conflicts += $rel } else { $picked += $rel }
    }
}
"== $Worker changes since $stamp"
$picked | ForEach-Object { "  merge    $_" }
$conflicts | ForEach-Object { "  CONFLICT $_ (main changed too - merge by hand)" }
if ($DryRun -or -not $picked) { return }

foreach ($rel in $picked) {
    $dst = Join-Path $main $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item (Join-Path $w $rel) $dst
    (Get-Item $dst).LastWriteTime = Get-Date
}
Push-Location $main
if ($picked -match '^configure\.py$|^config\\') { & $py configure.py --version GMSE01 | Out-Null }
$out = & $py -m ninja -j $Jobs 2>&1 | Out-String
($out -split "`n") | Select-String 'FAILED|error:|Game Code:|  All:' | Select-Object -Last 4 | ForEach-Object { $_.Line }
$dol = & build\tools\dtk.exe shasum -c config\GMSE01\build.sha1 2>&1
"DOL: $dol"
$ch = & $py -m ninja changes_all 2>&1 | Out-String
$lines = $ch -split "`n"
"-- unit changes:"
$lines | Where-Object { $_ -match 'mario/\S+\s+\|\s+fuzzy_match' } | ForEach-Object { "  " + ($_ -replace '\s+', ' ').Trim() }
"-- decreases:"
$dec = $lines | Where-Object { $_ -match '(\d+\.\d+)%\s+->\s+(\d+\.\d+)%' -and [double]$matches[2] -lt [double]$matches[1] }
if ($dec) { $dec | ForEach-Object { "  " + ($_ -replace '\s+', ' ').Trim() } } else { "  none" }
$realDec = $dec | Where-Object { $_ -notmatch 'isNerveCanGoToTalk|calcAnim__12TYoshiTongue' }
if ($realDec) {
    "!! decreases found - baseline NOT reset. Review, then fix or run 'python -m ninja baseline' yourself."
} elseif (-not $NoBaseline -and "$dol" -match 'OK') { & $py -m ninja baseline 2>&1 | Out-Null; "baseline reset" }
Pop-Location
