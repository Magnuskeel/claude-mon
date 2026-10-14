# Claude Code status line for claude-mon.
# Claude Code runs this on each status refresh and passes session JSON on stdin.
# It prints a short status line and, when the numbers change (or every 60 s),
# posts them to the display. Nothing runs in the background.
$DeviceUrl = 'http://claude-mon.local/usage'   # or e.g. 'http://192.168.0.50/usage'
$MonKey    = 'change-me-to-something-random'   # same as MON_KEY in firmware secrets.h

$ErrorActionPreference = 'SilentlyContinue'
$raw = [Console]::In.ReadToEnd()
$data = $raw | ConvertFrom-Json

function To-Unix($v) {
  if ($null -eq $v) { return $null }
  if ($v -is [string]) { return [DateTimeOffset]::Parse($v).ToUnixTimeSeconds() }
  return [int64]$v
}

function Window($w) {
  if ($null -eq $w) { return $null }
  return @{ pct = [double]$w.used_percentage; resets_at = (To-Unix $w.resets_at) }
}

$rl = $data.rate_limits
$fiveHour = Window $rl.five_hour
$sevenDay = Window $rl.seven_day

$parts = @()
if ($data.model.display_name) { $parts += $data.model.display_name }
if ($fiveHour) { $parts += ('5h {0:N0}%' -f $fiveHour.pct) }
if ($sevenDay) { $parts += ('7d {0:N0}%' -f $sevenDay.pct) }
Write-Output ($parts -join ' | ')

$tmp = [IO.Path]::GetTempPath()
if (-not $rl) {
  # Keep one sample so we can see what Claude Code actually sends.
  $raw | Set-Content (Join-Path $tmp 'claude-mon-statusline-debug.json')
  return
}

$body = @{ five_hour = $fiveHour; seven_day = $sevenDay } | ConvertTo-Json -Compress
$stateFile = Join-Path $tmp 'claude-mon-last.txt'
if (Test-Path $stateFile) {
  $prev = (Get-Content $stateFile -Raw).Trim()
  $age = ((Get-Date) - (Get-Item $stateFile).LastWriteTime).TotalSeconds
  if (($prev -eq $body -or $prev -eq 'FAILED') -and $age -lt 60) { return }
}

try {
  Invoke-RestMethod -Uri $DeviceUrl -Method Post -Body $body -ContentType 'application/json' `
    -Headers @{ 'X-Claude-Mon-Key' = $MonKey } -TimeoutSec 2 | Out-Null
  $body | Set-Content $stateFile
} catch {
  # Device offline: back off for 60 s so the status line stays fast.
  'FAILED' | Set-Content $stateFile
}
