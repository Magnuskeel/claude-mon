# Shared by the hook and usage scripts: settings and a fast, fail-quiet POST.
$ErrorActionPreference = 'SilentlyContinue'
$DeviceUrl = 'http://claude-mon.local'
$MonKey = ''
$configFile = Join-Path $PSScriptRoot 'config.ps1'
if (Test-Path $configFile) { . $configFile }
$DeviceUrl = ($DeviceUrl -replace '/usage$', '').TrimEnd('/')
$OfflineFile = Join-Path ([IO.Path]::GetTempPath()) 'claude-mon-offline.txt'

function Get-ProjectName($dir) {
  if ($dir) { return Split-Path $dir -Leaf }
  return ''
}

function Write-ClaudeMonConfig([string]$Path, [string]$Url, [string]$Key) {
  @(
    '# Written by the claude-mon installer.'
    "`$DeviceUrl = '$Url'"
    "`$MonKey    = '$Key'"
  ) | Set-Content -Path $Path -Encoding ASCII
}

# Lets the board show the clock in this PC's time zone, DST included.
function Get-UtcOffset { return [int][TimeZoneInfo]::Local.GetUtcOffset([DateTime]::Now).TotalSeconds }

function Invoke-Board([string]$Base, [string]$Path, [string]$Json) {
  Invoke-RestMethod -Uri "$Base$Path" -Method Post -ContentType 'application/json' `
    -Body ([Text.Encoding]::UTF8.GetBytes($Json)) -Headers @{ 'X-Claude-Mon-Key' = $MonKey } `
    -TimeoutSec 2 | Out-Null
}

# If the board's IP changed, find it again by name and remember the new address.
function Find-BoardAgain {
  try {
    $ip = [Net.Dns]::GetHostAddresses('claude-mon.local') |
      Where-Object { $_.AddressFamily -eq 'InterNetwork' } | Select-Object -First 1
    if (-not $ip) { return $null }
    $base = "http://$ip"
    if ($base -eq $DeviceUrl) { return $null }
    $info = Invoke-RestMethod -Uri "$base/info" -TimeoutSec 2
    if ($info.name -ne 'claude-mon') { return $null }
    Write-ClaudeMonConfig $configFile $base $MonKey
    return $base
  } catch { return $null }
}

# Posts JSON to the board. If the board was unreachable in the last 60 s, skips
# the attempt so Claude Code never waits on an offline device.
function Send-ClaudeMon([string]$Path, [string]$Json) {
  if ((Test-Path $OfflineFile) -and ((Get-Date) - (Get-Item $OfflineFile).LastWriteTime).TotalSeconds -lt 60) {
    return $false
  }
  try {
    Invoke-Board $DeviceUrl $Path $Json
    Remove-Item $OfflineFile -ErrorAction SilentlyContinue
    return $true
  } catch {
    $base = Find-BoardAgain
    if ($base) {
      try { Invoke-Board $base $Path $Json; return $true } catch {}
    }
    'offline' | Set-Content $OfflineFile
    return $false
  }
}

# ---------------------------------------------------------------------------
# Usage (scheduled task) and context (hooks).

$StateDir = [IO.Path]::GetTempPath()

function Write-ClaudeMonLog([string]$Line) {
  $log = Join-Path $StateDir 'claude-mon-hook.log'
  $lines = @()
  if (Test-Path $log) { $lines = @(Get-Content $log | Select-Object -Last 30) }
  $lines += "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $Line"
  $lines | Set-Content $log
}

function ConvertTo-UnixTime($v) {
  if ($null -eq $v -or $v -eq '') { return $null }
  if ($v -is [datetime]) { return ([DateTimeOffset]$v.ToUniversalTime()).ToUnixTimeSeconds() }
  if ($v -is [string]) { return [DateTimeOffset]::Parse($v).ToUnixTimeSeconds() }
  return [int64]$v
}

# A window with no usage yet (e.g. just after a reset) comes back empty: that's 0%.
function Get-UsageWindow($w) {
  if ($null -eq $w -or $null -eq $w.utilization) { return @{ pct = 0.0; resets_at = $null } }
  return @{ pct = [double]$w.utilization; resets_at = (ConvertTo-UnixTime $w.resets_at) }
}

function Test-LoginExpired($oauth) {
  # Treat logins expiring within 5 minutes as expired, so a lookup never races the expiry.
  return $oauth.expiresAt -and [int64]$oauth.expiresAt -lt [DateTimeOffset]::UtcNow.AddMinutes(5).ToUnixTimeMilliseconds()
}

# The desktop app keeps its own login, so the terminal login this script reads
# goes stale when only the desktop app is used. Running the Claude Code command
# line once makes it renew that login the normal way (a one-word Haiku prompt,
# a few tokens). At most once every 30 minutes; claude-mon's hooks ignore it.
function Update-ClaudeLogin {
  $stamp = Join-Path $StateDir 'claude-mon-login-renewed.txt'
  if ((Test-Path $stamp) -and ((Get-Date) - (Get-Item $stamp).LastWriteTime).TotalMinutes -lt 30) {
    Write-ClaudeMonLog 'usage: login expired; renewal tried recently, waiting'
    return $false
  }
  'renewing' | Set-Content $stamp

  # npm installs several 'claude' launchers (.ps1, .cmd, and an extensionless shell
  # script); only some can be started directly, so pick by extension.
  $claude = $null
  foreach ($name in 'claude.exe', 'claude.cmd', 'claude.ps1') {
    $found = Get-Command $name -CommandType Application, ExternalScript -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $claude = $found.Source; break }
  }
  if (-not $claude) {
    foreach ($c in "$env:USERPROFILE\.local\bin\claude.exe", "$env:APPDATA\npm\claude.cmd", "$env:APPDATA\npm\claude.ps1") {
      if (Test-Path $c) { $claude = $c; break }
    }
  }
  if (-not $claude) {
    Write-ClaudeMonLog 'usage: login expired and the claude command line was not found; run claude once in a terminal'
    return $false
  }
  $file = $claude
  $claudeArgs = @('-p', '"Reply with OK"', '--model', 'haiku')
  if ($claude -like '*.ps1') {
    $file = 'powershell.exe'
    $claudeArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$claude`"") + $claudeArgs
  }

  $env:CLAUDE_MON_SKIP = '1'  # inherited by Claude Code's hooks, which then do nothing
  $out = Join-Path $StateDir 'claude-mon-renew-output.txt'
  try {
    $p = Start-Process -FilePath $file -ArgumentList $claudeArgs `
      -WorkingDirectory $StateDir -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    if (-not $p.WaitForExit(40000)) { $p.Kill(); Write-ClaudeMonLog 'usage: login renewal timed out'; return $false }
  } catch {
    # Claude Code never started, so no tokens were spent: allow a retry next run.
    Remove-Item $stamp -ErrorAction SilentlyContinue
    Write-ClaudeMonLog "usage: couldn't start $claude : $($_.Exception.Message)"
    return $false
  } finally {
    Remove-Item Env:\CLAUDE_MON_SKIP -ErrorAction SilentlyContinue
  }
  if ($p.ExitCode -ne 0) {
    $err = (Get-Content "$out.err" -ErrorAction SilentlyContinue | Select-Object -First 2) -join ' '
    Write-ClaudeMonLog "usage: login renewal via $claude exited $($p.ExitCode): $err"
  } else {
    Write-ClaudeMonLog "usage: renewed login via $claude"
  }
  return $true
}

# Reads 5-hour and weekly usage the way Claude Code's /usage does, using Claude
# Code's own login on this PC. Never touches the login itself; if it has expired,
# Claude Code is asked to renew it. At most once a minute.
function Get-ClaudeUsage {
  $stamp = Join-Path $StateDir 'claude-mon-usage-checked.txt'
  if ((Test-Path $stamp) -and ((Get-Date) - (Get-Item $stamp).LastWriteTime).TotalSeconds -lt 60) { return $null }
  'checked' | Set-Content $stamp

  $home_ = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE '.claude' }
  $credFile = Join-Path $home_ '.credentials.json'
  if (-not (Test-Path $credFile)) { Write-ClaudeMonLog "usage: no login file at $credFile"; return $null }
  $oauth = (Get-Content $credFile -Raw | ConvertFrom-Json).claudeAiOauth
  if (-not $oauth.accessToken) { Write-ClaudeMonLog 'usage: no Claude login in credentials'; return $null }
  if (Test-LoginExpired $oauth) {
    if (-not (Update-ClaudeLogin)) { return $null }
    $oauth = (Get-Content $credFile -Raw | ConvertFrom-Json).claudeAiOauth
    if (Test-LoginExpired $oauth) { Write-ClaudeMonLog 'usage: login still expired after renewal'; return $null }
  }

  $url = if ($env:CLAUDE_MON_USAGE_URL) { $env:CLAUDE_MON_USAGE_URL } else { 'https://api.anthropic.com/api/oauth/usage' }
  try {
    $r = Invoke-RestMethod -Uri $url -TimeoutSec 4 -Headers @{
      Authorization    = "Bearer $($oauth.accessToken)"
      'anthropic-beta' = 'oauth-2025-04-20'
    }
  } catch {
    Write-ClaudeMonLog "usage: request failed: $($_.Exception.Message)"
    return $null
  }
  $names = @($r.PSObject.Properties.Name)
  if ('five_hour' -notin $names -and 'seven_day' -notin $names) {
    # Unknown response shape: keep a copy so it can be supported.
    $r | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $StateDir 'claude-mon-usage-debug.json')
    Write-ClaudeMonLog 'usage: unrecognised response, saved claude-mon-usage-debug.json'
    return $null
  }
  return @{ five_hour = (Get-UsageWindow $r.five_hour); seven_day = (Get-UsageWindow $r.seven_day) }
}

# "claude-opus-5-5" -> "Opus 5.5"
function Get-ModelDisplayName([string]$id) {
  if ($id -match 'claude-([a-z]+)-(\d+)-(\d+)') {
    return (Get-Culture).TextInfo.ToTitleCase($Matches[1]) + " $($Matches[2]).$($Matches[3])"
  }
  return $id
}

# Context used by the main conversation: the token count of the latest response
# in the session transcript (subagent turns excluded), as the status line shows.
# Returns $null and sets $script:ContextMiss to the reason when nothing is found.
function Read-TranscriptContext([string]$Path) {
  if (-not $Path) { $script:ContextMiss = 'hook gave no transcript_path'; return $null }
  if (-not (Test-Path -LiteralPath $Path)) { $script:ContextMiss = "transcript not found: $Path"; return $null }
  $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
  try {
    $fs.Seek([Math]::Max(0, $fs.Length - 512KB), 'Begin') | Out-Null
    $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
  } finally { $fs.Close() }
  $lines = $text -split "`n"
  for ($i = $lines.Count - 1; $i -ge 0; $i--) {
    $line = $lines[$i]
    if ($line -notmatch '"usage"' -or $line -notmatch '"assistant"') { continue }
    try { $entry = $line | ConvertFrom-Json } catch { continue }
    if ($entry.isSidechain -or -not $entry.message.usage) { continue }
    $u = $entry.message.usage
    $used = [int64]$u.input_tokens + [int64]$u.cache_creation_input_tokens + [int64]$u.cache_read_input_tokens
    if ($used -le 0) { continue }
    $size = if ($used -gt 200000 -or [string]$entry.message.model -match '1m') { 1000000 } else { 200000 }
    return @{
      context = @{ pct = [Math]::Round(100.0 * $used / $size, 1); used = $used; size = $size }
      model   = Get-ModelDisplayName ([string]$entry.message.model)
    }
  }
  $script:ContextMiss = "no response with token counts yet ($($lines.Count) lines read)"
  return $null
}

# Claude Code can run the Stop hook a moment before it writes the final reply to
# the transcript, so look again briefly before giving up.
function Get-TranscriptContext([string]$Path) {
  for ($try = 0; $try -lt 4; $try++) {
    $ctx = Read-TranscriptContext $Path
    if ($ctx -or $script:ContextMiss -like 'hook gave*') { return $ctx }
    Start-Sleep -Milliseconds 500
  }
  return $null
}

# Local Claude Code sessions active in the last 2 hours (newest last), read from
# the transcripts Claude Code keeps under ~/.claude/projects. Used by the
# scheduled task so context stays current during long replies and comes back
# after the board restarts.
function Get-RecentSessions([int]$Max = 4) {
  $root = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE '.claude' }
  $projects = Join-Path $root 'projects'
  if (-not (Test-Path $projects)) { return @() }
  $cutoff = (Get-Date).AddHours(-2)
  $files = Get-ChildItem -Path ([IO.Path]::Combine($projects, '*', '*.jsonl')) -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt $cutoff } | Sort-Object LastWriteTime -Descending | Select-Object -First $Max
  $out = @()
  foreach ($f in ($files | Sort-Object LastWriteTime)) {
    $ctx = Read-TranscriptContext $f.FullName
    if (-not $ctx) { continue }
    # The session's working folder is recorded on its transcript lines.
    $project = ''
    $tail = Get-Content -LiteralPath $f.FullName -Tail 20 -ErrorAction SilentlyContinue
    foreach ($line in $tail) {
      if ($line -match '"cwd"\s*:\s*"((?:[^"\\]|\\.)*)"') { $project = Get-ProjectName ($Matches[1] -replace '\\\\', '\') }
    }
    $out += @{
      session_id    = $f.BaseName
      project       = $project
      context       = $ctx.context
      model         = $ctx.model
      last_activity = ([DateTimeOffset]$f.LastWriteTimeUtc).ToUnixTimeSeconds()
    }
  }
  return $out
}
