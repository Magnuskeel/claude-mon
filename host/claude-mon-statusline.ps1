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

# Context window of this Claude Code session. Newer versions send used_percentage;
# otherwise work it out from the last request's token counts.
$context = $null
$cw = $data.context_window
if ($cw) {
  $size = [int64]$cw.context_window_size
  $u = $cw.current_usage
  $used = [int64]$u.input_tokens + [int64]$u.cache_creation_input_tokens + [int64]$u.cache_read_input_tokens
  $pct = $cw.used_percentage
  if ($null -eq $pct -and $size -gt 0) { $pct = 100.0 * $used / $size }
  if ($null -ne $pct) { $context = @{ pct = [double]$pct; used = $used; size = $size } }
}
$project = if ($data.workspace.current_dir) { Split-Path $data.workspace.current_dir -Leaf } else { '' }

$parts = @()
if ($data.model.display_name) { $parts += $data.model.display_name }
if ($context) { $parts += ('ctx {0:N0}%' -f $context.pct) }
if ($fiveHour) { $parts += ('5h {0:N0}%' -f $fiveHour.pct) }
if ($sevenDay) { $parts += ('7d {0:N0}%' -f $sevenDay.pct) }
Write-Output ($parts -join ' | ')

$tmp = [IO.Path]::GetTempPath()
if (-not $rl) {
  # Keep one sample so we can see what Claude Code actually sends.
  $raw | Set-Content (Join-Path $tmp 'claude-mon-statusline-debug.json')
  if (-not $context) { return }
}

$body = @{
  five_hour = $fiveHour; seven_day = $sevenDay; context = $context
  model = [string]$data.model.display_name; project = $project
} | ConvertTo-Json -Compress
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
