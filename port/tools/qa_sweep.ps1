# Visual QA sweep + pipeline-seed generation for the Aurora build (w6).
#
#   powershell -File port\tools\qa_sweep.ps1 [-CacheDir cache_seed] [-Only plaza0,bianco0] [-Seconds 72]
#
# Runs port\build_aurora\sms_port.exe once per entry below (SMS_PORT_WARP + a scripted pad
# sequence: advance dialogs, run and spray, turn, jump, dive, pause), all sharing one Aurora cache
# directory (port\build_aurora\<CacheDir>), and saves half-size PNG screenshots to
# port\build_aurora\qa_sweep\<entry>_<frame>.png (logs in port\build_aurora\sweep_logs).
# Afterwards `python port\tools\make_pipeline_seed.py port\build_aurora\<CacheDir>` turns the
# cache into port\data\initial_pipeline_cache.db. Delete <CacheDir> first for a clean seed.
# Only the sms_port.exe started here is ever killed.
param([string]$CacheDir = "cache_seed", [string]$Only = "", [int]$Seconds = 72)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root "port\build_aurora"
$qa = "$out\qa_sweep"; New-Item -ItemType Directory -Force $qa, "$out\sweep_logs" | Out-Null
Add-Type -AssemblyName System.Drawing
if (-not $env:SMS_PORT_DISC) {
  foreach ($up in @((Split-Path -Parent $root), (Split-Path -Parent (Split-Path -Parent $root)))) {
    if (Test-Path "$up\dolphin_test\files") { $env:SMS_PORT_DISC = "$up\dolphin_test\files"; break }
  }
}
$env:SMS_PORT_CACHE_DIR = "$out\$CacheDir"
$env:SMS_PORT_THP_SPEED = "8"
$game = "900-2100/50:A,2100-2500:S0+R,2500-2800:S90+R,2800-3000/30:A+S180,3000-3020:B,3100-3105:START,3190-3195:START"
$shots = "1500,2050,2450,2900,3150"
$entries = @(
  @{t="title"; w=""; p="2000:START,2200:A,2500:A"; s="1800,2150,2400,2700,3000"},
  @{t="airstrip0"; w="0,0"}, @{t="airstrip1"; w="0,1"},
  @{t="plaza0"; w="1,0"}, @{t="plaza1"; w="1,1"}, @{t="plaza5"; w="1,5"},
  @{t="bianco0"; w="2,0"}, @{t="bianco3"; w="2,3"},
  @{t="ricco0"; w="3,0"}, @{t="ricco2"; w="3,2"},
  @{t="gelato0"; w="4,0"}, @{t="gelato2"; w="4,2"},
  @{t="pinnabeach0"; w="5,0"}, @{t="pinnapark0"; w="13,0"}, @{t="pinnapark2"; w="13,2"},
  @{t="sirena0"; w="6,0"}, @{t="sirena2"; w="6,2"}, @{t="casino0"; w="14,0"},
  @{t="hotel0"; w="7,0"}, @{t="hotel2"; w="7,2"},
  @{t="pianta0"; w="8,0"}, @{t="pianta3"; w="8,3"},
  @{t="noki0"; w="9,0"}, @{t="noki2"; w="9,2"}, @{t="undersea"; w="16,0"},
  @{t="corona"; w="60,0"}, @{t="ex20"; w="20,0"}, @{t="ex21"; w="21,0"},
  @{t="biancoboss"; w="55,0"}, @{t="mareboss"; w="57,0"}, @{t="ricco8"; w="59,0"}
)
foreach ($e in $entries) {
  if ($Only -and ($Only -split ',') -notcontains $e.t) { continue }
  $pad = if ($e.p) { $e.p } else { $game }
  $sh = if ($e.s) { $e.s } else { $shots }
  if ($e.w) { $env:SMS_PORT_WARP = $e.w } else { Remove-Item Env:SMS_PORT_WARP -ErrorAction SilentlyContinue }
  $env:SMS_PORT_PADSCRIPT = $pad
  $env:SMS_PORT_SHOT = $sh
  Get-ChildItem "$out\shot_*.bmp" -ErrorAction SilentlyContinue | Remove-Item
  $log = "$out\sweep_logs\$($e.t)"
  $p = Start-Process -FilePath "$out\sms_port.exe" -WorkingDirectory $out -PassThru -RedirectStandardOutput "$log.log" -RedirectStandardError "$log.err"
  if (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force; $st = "ok" } else { $st = "EXITED $($p.ExitCode)" }
  foreach ($b in Get-ChildItem "$out\shot_*.bmp" -ErrorAction SilentlyContinue) {
    $img = [System.Drawing.Image]::FromFile($b.FullName)
    $bmp = New-Object System.Drawing.Bitmap $img, ([int]($img.Width / 2)), ([int]($img.Height / 2))
    $bmp.Save("$qa\$($e.t)_$($b.BaseName -replace 'shot_','').png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose(); $img.Dispose()
  }
  $fatal = (Select-String -Path "$log.err" -Pattern 'FATAL|CRASH' | Select-Object -First 1).Line
  $warn = (Select-String -Path "$log.log" -Pattern 'aurora (ERROR|WARNING)').Count
  $m = Select-String -Path "$log.err" -Pattern 'frame 600 \(retrace (\d+)\)' | Select-Object -First 1
  $f600 = if ($m) { $m.Matches[0].Groups[1].Value } else { "-" }
  "{0,-12} {1,-8} fatal=[{2}] auroraWarn={3} retraceAtFrame600={4}" -f $e.t, $st, $fatal, $warn, $f600
}
Remove-Item Env:SMS_PORT_WARP, Env:SMS_PORT_PADSCRIPT, Env:SMS_PORT_SHOT -ErrorAction SilentlyContinue
