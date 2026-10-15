# Claude Code hook for claude-mon. Register it for UserPromptSubmit, Notification,
# Stop, SessionStart and SessionEnd. Forwards the event to the board and prints
# nothing (UserPromptSubmit output would be added to Claude's context).
. (Join-Path $PSScriptRoot 'claude-mon-common.ps1')

$data = [Console]::In.ReadToEnd() | ConvertFrom-Json
if (-not $data.session_id) { exit 0 }

$body = [ordered]@{
  session_id        = [string]$data.session_id
  event             = [string]$data.hook_event_name
  project           = Get-ProjectName $data.cwd
  message           = [string]$data.message
  notification_type = [string]$data.notification_type
  utc_offset        = Get-UtcOffset
} | ConvertTo-Json -Compress

[void](Send-ClaudeMon '/hook' $body)
exit 0
