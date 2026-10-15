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
