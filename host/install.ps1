# claude-mon installer for Windows (PowerShell 5.1+).
#
#   irm https://magnuskeel.github.io/claude-mon/install.ps1 | iex
#
# What it changes, and nothing else:
#   - copies four small scripts to %LOCALAPPDATA%\claude-mon
#   - adds a status line and five hooks to %USERPROFILE%\.claude\settings.json
#     (backed up first; existing hooks are kept)
# No services, scheduled tasks, startup entries or drivers. Undo with:
#   powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\claude-mon\uninstall.ps1"

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$BaseUrl = if ($env:CLAUDE_MON_BASE_URL) { $env:CLAUDE_MON_BASE_URL } else { 'https://magnuskeel.github.io/claude-mon' }
$InstallDir = Join-Path $env:LOCALAPPDATA 'claude-mon'
$SettingsPath = Join-Path $env:USERPROFILE '.claude\settings.json'
$Scripts = 'claude-mon-common.ps1', 'claude-mon-hook.ps1', 'claude-mon-statusline.ps1', 'uninstall.ps1'
$HookEvents = 'UserPromptSubmit', 'Notification', 'Stop', 'SessionStart', 'SessionEnd'

function Say($msg) { Write-Host "  $msg" }
function Step($n, $msg) { Write-Host ''; Write-Host " [$n/4] $msg" -ForegroundColor Cyan }
function Ok($msg) { Write-Host "  OK  $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host ''; Write-Host "  $msg" -ForegroundColor Red; Write-Host ''; throw 'claude-mon install stopped.' }

function Get-BoardInfo([string]$Base) {
  try {
    $info = Invoke-RestMethod -Uri "$Base/info" -TimeoutSec 3
    if ($info.name -eq 'claude-mon') { return $info }
  } catch {}
  return $null
}

# Looks for the board by name, then by probing every address on this PC's local networks.
function Find-Board {
  if ($env:CLAUDE_MON_DEVICE) {
    $base = "http://$($env:CLAUDE_MON_DEVICE)"
    if (Get-BoardInfo $base) { return $base }
  }
  try {
    $ip = [Net.Dns]::GetHostAddresses('claude-mon.local') |
      Where-Object { $_.AddressFamily -eq 'InterNetwork' } | Select-Object -First 1
    if ($ip -and (Get-BoardInfo "http://$ip")) { return "http://$ip" }
  } catch {}

  Say 'Scanning your network...'
  Add-Type -AssemblyName System.Net.Http
  $client = New-Object System.Net.Http.HttpClient
  $client.Timeout = [TimeSpan]::FromSeconds(2)
  $prefixes = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
    ForEach-Object { ($_.IPAddress -split '\.')[0..2] -join '.' } | Select-Object -Unique
  foreach ($prefix in $prefixes) {
    $tasks = @{}
    foreach ($i in 1..254) { $tasks["$prefix.$i"] = $client.GetStringAsync("http://$prefix.$i/info") }
    try { [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($tasks.Values)) } catch {}
    foreach ($entry in $tasks.GetEnumerator()) {
      if ($entry.Value.Status -eq 'RanToCompletion' -and $entry.Value.Result -match '"name"\s*:\s*"claude-mon"') {
        return "http://$($entry.Key)"
      }
    }
  }
  return $null
}

function Set-Prop($obj, [string]$name, $value) {
  if ($obj.PSObject.Properties[$name]) { $obj.$name = $value }
  else { $obj | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}

function Test-OurHookGroup($group) {
  foreach ($h in @($group.hooks)) { if ([string]$h.command -match 'claude-mon') { return $true } }
  return $false
}

# ---------------------------------------------------------------------------

Write-Host ''
Write-Host ' claude-mon installer' -ForegroundColor Yellow
Say 'This copies 4 scripts to'
Say "  $InstallDir"
Say 'and adds a status line and hooks to'
Say "  $SettingsPath (backed up first)."
Say 'Nothing runs in the background and nothing starts with Windows.'
$answer = Read-Host '  Continue? [Y/n]'
if ($answer -and $answer -notmatch '^[Yy]') { Say 'Cancelled.'; return }

# Already installed and paired? Re-running just updates the scripts and settings.
$device = $null
$key = $null
$existing = Join-Path $InstallDir 'config.ps1'
if (Test-Path $existing) {
  $DeviceUrl = $null; $MonKey = $null
  . $existing
  if ($DeviceUrl -and $MonKey) {
    try {
      $probe = @{ session_id = ''; event = 'Test'; project = $env:COMPUTERNAME } | ConvertTo-Json -Compress
      Invoke-RestMethod -Uri "$DeviceUrl/hook" -Method Post -ContentType 'application/json' -Body $probe `
        -Headers @{ 'X-Claude-Mon-Key' = $MonKey } -TimeoutSec 4 | Out-Null
      $device = $DeviceUrl
      $key = $MonKey
    } catch {}
  }
}

# 1. Find the board ------------------------------------------------------------
Step 1 'Finding your claude-mon board'
if (-not $device) { $device = Find-Board }
while (-not $device) {
  Say "Couldn't find it automatically. Make sure the board is on and connected to Wi-Fi."
  $typed = Read-Host '  Enter the address shown on the board (e.g. 192.168.0.42), or press Enter to quit'
  if (-not $typed) { Fail 'No board found.' }
  $typed = $typed.Trim() -replace '^https?://', '' -replace '/.*$', ''
  if (Get-BoardInfo "http://$typed") { $device = "http://$typed" }
}
$info = Get-BoardInfo $device
Ok "Found claude-mon $($info.version) at $device"

# 2. Pair ----------------------------------------------------------------------
Step 2 'Pairing'
if ($key) {
  Ok 'Already paired with this board'
} elseif (-not $info.pairing) {
  Say 'Hold your finger on the board''s screen for 3 seconds until a code appears.'
}
for ($try = 1; -not $key -and $try -le 5; $try++) {
  $code = (Read-Host '  Enter the 6-digit code shown on the board') -replace '\D', ''
  try {
    $body = @{ code = $code; pc = $env:COMPUTERNAME } | ConvertTo-Json -Compress
    $resp = Invoke-RestMethod -Uri "$device/pair" -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 5
    $key = $resp.key
  } catch {
    $status = $_.Exception.Response.StatusCode.value__
    if ($status -eq 403) { Say 'The board isn''t showing a code. Hold the screen for 3 seconds, then try again.' }
    else { Say 'That code didn''t match. Check the screen and try again.' }
  }
}
if (-not $key) { Fail 'Pairing failed.' }
if ($try -gt 1) { Ok 'Paired' }

# 3. Install scripts -----------------------------------------------------------
Step 3 'Installing scripts'
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$localSource = if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot 'claude-mon-hook.ps1'))) { $PSScriptRoot } else { $null }
foreach ($f in $Scripts) {
  $dest = Join-Path $InstallDir $f
  if ($localSource) { Copy-Item (Join-Path $localSource $f) $dest -Force }
  else { Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/$f" -OutFile $dest }
}
. (Join-Path $InstallDir 'claude-mon-common.ps1')
$ErrorActionPreference = 'Stop'
Write-ClaudeMonConfig (Join-Path $InstallDir 'config.ps1') $device $key
Remove-Item (Join-Path ([IO.Path]::GetTempPath()) 'claude-mon-offline.txt') -ErrorAction SilentlyContinue
Ok "Scripts in $InstallDir"

# 4. Claude Code settings ------------------------------------------------------
Step 4 'Updating Claude Code settings'
$settings = [pscustomobject]@{}
if (Test-Path $SettingsPath) {
  $raw = Get-Content $SettingsPath -Raw
  $backup = "$SettingsPath.claude-mon-backup-$(Get-Date -Format yyyyMMdd-HHmmss)"
  Copy-Item $SettingsPath $backup
  Say "Backup: $backup"
  if ($raw -and $raw.Trim()) {
    try { $settings = $raw | ConvertFrom-Json }
    catch { Fail "Couldn't read $SettingsPath (is it valid JSON?). Nothing was changed there." }
  }
} else {
  New-Item -ItemType Directory -Force -Path (Split-Path $SettingsPath) | Out-Null
}

$dirForward = $InstallDir -replace '\\', '/'
$run = 'powershell -NoProfile -ExecutionPolicy Bypass -File'
$statusCmd = "$run `"$dirForward/claude-mon-statusline.ps1`""
$hookCmd = "$run `"$dirForward/claude-mon-hook.ps1`""

$current = $settings.statusLine
$replaceStatus = $true
if ($current -and [string]$current.command -and [string]$current.command -notmatch 'claude-mon') {
  Say "You already have a status line: $($current.command)"
  $a = Read-Host '  Replace it with claude-mon''s? (it shows model, context and usage) [Y/n]'
  if ($a -and $a -notmatch '^[Yy]') {
    $replaceStatus = $false
    Say 'Kept your status line. The usage rings will stay empty without it.'
  }
}
if ($replaceStatus) {
  Set-Prop $settings 'statusLine' ([pscustomobject]@{ type = 'command'; command = $statusCmd; padding = 0 })
}

if (-not $settings.hooks) { Set-Prop $settings 'hooks' ([pscustomobject]@{}) }
foreach ($ev in $HookEvents) {
  $groups = @(@($settings.hooks.$ev) | Where-Object { $_ -and -not (Test-OurHookGroup $_) })
  $groups += [pscustomobject]@{ hooks = @([pscustomobject]@{ type = 'command'; command = $hookCmd; timeout = 5 }) }
  Set-Prop $settings.hooks $ev $groups
}

$json = $settings | ConvertTo-Json -Depth 32
[IO.File]::WriteAllText($SettingsPath, $json, (New-Object Text.UTF8Encoding $false))
Ok 'Status line and hooks added'

# Say hello -------------------------------------------------------------------
try {
  $hello = @{ session_id = ''; event = 'Test'; project = $env:COMPUTERNAME; utc_offset = (Get-UtcOffset) } | ConvertTo-Json -Compress
  Invoke-RestMethod -Uri "$device/hook" -Method Post -ContentType 'application/json' -Body $hello `
    -Headers @{ 'X-Claude-Mon-Key' = $key } -TimeoutSec 5 | Out-Null
  Ok 'The board should now say "Connected"'
} catch {
  Say 'Installed, but the test message to the board failed. It will retry when Claude Code runs.'
}

Write-Host ''
Write-Host ' All set. Restart Claude Code and send a message.' -ForegroundColor Yellow
Say 'To uninstall later:'
Say "  powershell -ExecutionPolicy Bypass -File `"$InstallDir\uninstall.ps1`""
Write-Host ''
