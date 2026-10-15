# Shared by the status line and hook scripts: settings and a fast, fail-quiet POST.
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
# Usage and context without the status line (the desktop app has none).

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

function Get-UsageWindow($w) {
  if ($null -eq $w -or $null -eq $w.utilization) { return $null }
  return @{ pct = [double]$w.utilization; resets_at = (ConvertTo-UnixTime $w.resets_at) }
}

# Reads 5-hour and weekly usage the way Claude Code's /usage does, using Claude
# Code's own login on this PC. Never refreshes the login (that's Claude Code's
# job); at most once a minute.
function Get-ClaudeUsage {
  $stamp = Join-Path $StateDir 'claude-mon-usage-checked.txt'
  if ((Test-Path $stamp) -and ((Get-Date) - (Get-Item $stamp).LastWriteTime).TotalSeconds -lt 60) { return $null }
  'checked' | Set-Content $stamp

  $home_ = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE '.claude' }
  $credFile = Join-Path $home_ '.credentials.json'
  if (-not (Test-Path $credFile)) { Write-ClaudeMonLog "usage: no login file at $credFile"; return $null }
  $oauth = (Get-Content $credFile -Raw | ConvertFrom-Json).claudeAiOauth
  if (-not $oauth.accessToken) { Write-ClaudeMonLog 'usage: no Claude login in credentials'; return $null }
  if ($oauth.expiresAt -and [int64]$oauth.expiresAt -lt [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) {
    Write-ClaudeMonLog 'usage: login expired; Claude Code will renew it'
    return $null
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
  $five = Get-UsageWindow $r.five_hour
  $week = Get-UsageWindow $r.seven_day
  if (-not $five -and -not $week) {
    # Unknown response shape: keep a copy so it can be supported.
    $r | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $StateDir 'claude-mon-usage-debug.json')
    Write-ClaudeMonLog 'usage: unrecognised response, saved claude-mon-usage-debug.json'
    return $null
  }
  return @{ five_hour = $five; seven_day = $week }
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
function Get-TranscriptContext([string]$Path) {
  if (-not $Path -or -not (Test-Path $Path)) { return $null }
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
  return $null
}
