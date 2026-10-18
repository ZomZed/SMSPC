# One-shot merge -> build -> smoke -> install for the PC port (run from the repo root).
#   powershell -File port\tools\ship.ps1 [-Worker w9] [-NoSmoke] [-Jobs 8] [-Only "1,0 2,0"]
# Faster than doing the steps by hand:
# - the GameCube build (ninja + shasum) only runs when src/, include/, libs/ or config/
#   changed since the last mario.dol;
# - the smoke test runs 8 scenes at a time instead of 3 (-Jobs), or only the scenes
#   given with -Only;
# - SMS_PC\sms_port.exe is refreshed at the end unless the player has the game open.
param([string]$Worker = "", [switch]$NoSmoke, [int]$Jobs = 8, [string]$Only = "")
$OnlyList = @($Only -split "[\s;]+" | Where-Object { $_ })
$ErrorActionPreference = "Stop"
$py = "$env:LOCALAPPDATA\Python\pythoncore-3.14-64\python.exe"
if (-not (Test-Path $py)) { $py = "python" }
$t0 = Get-Date
function Step($name) { Write-Host ("[ship {0,5:N0}s] {1}" -f ((Get-Date) - $t0).TotalSeconds, $name) }

if ($Worker) {
    Step "merge $Worker"
    $out = & $py port\tools\merge_check.py $Worker --all --apply --apply-new 2>&1 | Out-String
    if ($out -match "CONFLICT|merge by hand|check by hand") {
        Write-Host $out
        throw "merge needs a hand merge; nothing else done"
    }
    ($out -split "`n") | Where-Object { $_ -match "^--" -and $_ -notmatch "STALE|new in main" } | Write-Host
}

$dol = "build\GMSE01\mario.dol"
$newest = Get-ChildItem src, include, libs, config -Recurse -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not (Test-Path $dol) -or $newest.LastWriteTime -gt (Get-Item $dol).LastWriteTime) {
    Step "GameCube build ($($newest.Name) changed)"
    & $py -m ninja | Select-Object -Last 1 | Write-Host
    $sha = & build\tools\dtk.exe shasum -c config\GMSE01\build.sha1 2>&1 | Out-String
    Write-Host $sha.Trim()
    if ($sha -notmatch "OK") { throw "GameCube build no longer matches" }
} else {
    Step "GameCube build skipped (no shared source changed)"
}

Step "port build"
$exe = "port\build_aurora\sms_port.exe"
if (Test-Path $exe) {
    try { [IO.File]::Open($exe, 'Open', 'ReadWrite', 'None').Close() }
    catch { Move-Item $exe ("port\build_aurora\sms_port.old_{0}.exe" -f (Get-Date -Format HHmmss)) }
}
$b = & $py port\tools\build_aurora.py 2>&1 | Select-Object -Last 1 | Out-String
Write-Host $b.Trim()
if ($b -notmatch "exe: yes") { throw "port build failed (see port\build_aurora\build.log)" }

if (-not $NoSmoke) {
    Step "smoke (-j $Jobs)"
    $args2 = @("port\tools\smoke_all.py", "--quick", "--no-build", "-j", "$Jobs")
    if ($OnlyList.Count) { $args2 += "--only"; $args2 += $OnlyList }
    $s = & $py @args2 2>&1 | Out-String
    ($s -split "`n") | Where-Object { $_ -match "FAIL |STUCK |PASS \d+" } | Write-Host
}

$running = Get-Process sms_port* -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\SMS_PC\*' }
if ($running) { Step "SMS_PC not updated: the game is open" } else {
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $exe))
    $pe = [BitConverter]::ToInt32($bytes, 0x3C); $bytes[$pe + 24 + 68] = 2 # GUI subsystem: no console
    [IO.File]::WriteAllBytes((Join-Path (Resolve-Path ..\SMS_PC) "sms_port.exe"), $bytes)
    Step "SMS_PC updated"
}
Step "done"
