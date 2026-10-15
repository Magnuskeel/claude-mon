# Removes claude-mon from this PC: its hooks and status line in Claude Code's
# settings.json (other settings untouched), the "claude-mon usage" scheduled task
# and the %LOCALAPPDATA%\claude-mon folder.
$ErrorActionPreference = 'Stop'
$InstallDir = Join-Path $env:LOCALAPPDATA 'claude-mon'
$SettingsPath = Join-Path $env:USERPROFILE '.claude\settings.json'

function Test-OurHookGroup($group) {
  foreach ($h in @($group.hooks)) { if ([string]$h.command -match 'claude-mon') { return $true } }
  return $false
}

if (Test-Path $SettingsPath) {
  $raw = Get-Content $SettingsPath -Raw
  if ($raw -and $raw.Trim()) {
    $settings = $raw | ConvertFrom-Json
    Copy-Item $SettingsPath "$SettingsPath.claude-mon-uninstall-backup-$(Get-Date -Format yyyyMMdd-HHmmss)"

    if ($settings.statusLine -and [string]$settings.statusLine.command -match 'claude-mon') {
      $settings.PSObject.Properties.Remove('statusLine')
    }
    if ($settings.hooks) {
      foreach ($prop in @($settings.hooks.PSObject.Properties)) {
        $kept = @(@($prop.Value) | Where-Object { $_ -and -not (Test-OurHookGroup $_) })
        if ($kept.Count) { $settings.hooks.($prop.Name) = $kept }
        else { $settings.hooks.PSObject.Properties.Remove($prop.Name) }
      }
      if (-not @($settings.hooks.PSObject.Properties).Count) { $settings.PSObject.Properties.Remove('hooks') }
    }
    $json = $settings | ConvertTo-Json -Depth 32
    [IO.File]::WriteAllText($SettingsPath, $json, (New-Object Text.UTF8Encoding $false))
    Write-Host '  Removed claude-mon from Claude Code settings.'
  }
}

if (Get-Command Unregister-ScheduledTask -ErrorAction SilentlyContinue) {
  if (Get-ScheduledTask -TaskName 'claude-mon usage' -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName 'claude-mon usage' -Confirm:$false
    Write-Host '  Removed the claude-mon usage scheduled task.'
  }
}

$tmp = [IO.Path]::GetTempPath()
'claude-mon-offline.txt', 'claude-mon-last.txt', 'claude-mon-statusline-debug.json', 'claude-mon-hook.log',
  'claude-mon-usage-checked.txt', 'claude-mon-usage-debug.json' |
  ForEach-Object { Remove-Item (Join-Path $tmp $_) -ErrorAction SilentlyContinue }
Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host '  Deleted' $InstallDir
Write-Host '  claude-mon is uninstalled. Restart Claude Code to finish.'
