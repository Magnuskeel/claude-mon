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

# Posts JSON to the board. If the board was unreachable in the last 60 s, skips
# the attempt so Claude Code never waits on an offline device.
function Send-ClaudeMon([string]$Path, [string]$Json) {
  if ((Test-Path $OfflineFile) -and ((Get-Date) - (Get-Item $OfflineFile).LastWriteTime).TotalSeconds -lt 60) {
    return $false
  }
  try {
    Invoke-RestMethod -Uri "$DeviceUrl$Path" -Method Post -ContentType 'application/json' `
      -Body ([Text.Encoding]::UTF8.GetBytes($Json)) -Headers @{ 'X-Claude-Mon-Key' = $MonKey } `
      -TimeoutSec 2 | Out-Null
    Remove-Item $OfflineFile -ErrorAction SilentlyContinue
    return $true
  } catch {
    'offline' | Set-Content $OfflineFile
    return $false
  }
}
