# Run every 2 minutes by the "claude-mon usage" scheduled task (added by the
# installer). Sends:
#   - 5-hour and weekly usage: Anthropic's account-wide count (as /usage shows),
#     covering the desktop app, the terminal, cloud sessions and claude.ai;
#   - context for local sessions active in the last 2 hours, from their transcripts.
# Finishes in about a second.
. (Join-Path $PSScriptRoot 'claude-mon-common.ps1')

$offset = Get-UtcOffset
$usage = Get-ClaudeUsage
if ($usage) {
  $body = [ordered]@{ five_hour = $usage.five_hour; seven_day = $usage.seven_day; utc_offset = $offset } |
    ConvertTo-Json -Compress -Depth 5
  $ok = Send-ClaudeMon '/usage' $body
  Write-ClaudeMonLog "task: usage 5h=$($usage.five_hour.pct)% 7d=$($usage.seven_day.pct)% sent=$ok"
}

foreach ($s in Get-RecentSessions) {
  $s.utc_offset = $offset
  $ok = Send-ClaudeMon '/usage' ($s | ConvertTo-Json -Compress -Depth 5)
  Write-ClaudeMonLog "task: context $($s.project) $($s.context.pct)% sent=$ok"
}
exit 0
