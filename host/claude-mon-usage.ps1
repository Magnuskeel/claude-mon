# Sends 5-hour and weekly usage to the board. Run every 2 minutes by the
# "claude-mon usage" scheduled task (added by the installer). This is the board's
# only usage source: Anthropic's account-wide count, so it covers the desktop
# app, the terminal, cloud sessions and claude.ai. Finishes in about a second.
. (Join-Path $PSScriptRoot 'claude-mon-common.ps1')

$usage = Get-ClaudeUsage
if ($usage) {
  $body = [ordered]@{ five_hour = $usage.five_hour; seven_day = $usage.seven_day; utc_offset = Get-UtcOffset } |
    ConvertTo-Json -Compress -Depth 5
  $ok = Send-ClaudeMon '/usage' $body
  Write-ClaudeMonLog "task: usage 5h=$($usage.five_hour.pct)% 7d=$($usage.seven_day.pct)% sent=$ok"
}
exit 0
