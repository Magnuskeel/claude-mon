# Claude Code hook for claude-mon. Register it for UserPromptSubmit, Notification,
# Stop, SessionStart and SessionEnd. Forwards the event to the board; when Claude
# finishes a reply (Stop) or a session starts, also sends usage and context, since
# the desktop app has no status line. Prints nothing: UserPromptSubmit output
# would be added to Claude's context.
. (Join-Path $PSScriptRoot 'claude-mon-common.ps1')

$data = [Console]::In.ReadToEnd() | ConvertFrom-Json
if (-not $data.session_id) { exit 0 }
$hookEvent = [string]$data.hook_event_name
$project = Get-ProjectName $data.cwd

$body = [ordered]@{
  session_id        = [string]$data.session_id
  event             = $hookEvent
  project           = $project
  message           = [string]$data.message
  notification_type = [string]$data.notification_type
  utc_offset        = Get-UtcOffset
} | ConvertTo-Json -Compress
$sent = Send-ClaudeMon '/hook' $body
Write-ClaudeMonLog "$hookEvent $project sent=$sent"

if ($sent -and $hookEvent -in 'Stop', 'SessionStart') {
  $ctx = Get-TranscriptContext ([string]$data.transcript_path)
  $usage = Get-ClaudeUsage
  if ($ctx -or $usage) {
    $update = [ordered]@{ session_id = [string]$data.session_id; project = $project; utc_offset = Get-UtcOffset }
    if ($ctx) { $update.context = $ctx.context; $update.model = $ctx.model }
    if ($usage) { $update.five_hour = $usage.five_hour; $update.seven_day = $usage.seven_day }
    $ok = Send-ClaudeMon '/usage' ($update | ConvertTo-Json -Compress -Depth 5)
    Write-ClaudeMonLog "  usage=$([bool]$usage) context=$(if ($ctx) { $ctx.context.pct } else { '-' })% sent=$ok"
  }
}
exit 0
