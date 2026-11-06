param(
    [string]$Build,          # A, AB, ABC
    [int]$Count,
    [int[]]$Workers = @(2, 4),
    [int]$RunSec = 20
)
# Runs lock_logic ch.9 matrix inside the temporary worktree. Results go to %TEMP%\arkchimy_echo_test\deadlock_*
$wt = 'C:\_Arkchimy_deadlock'
$cfg = "$wt\networkLibProject\server.cfg"
$utf8 = New-Object Text.UTF8Encoding($false)
$outRoot = Join-Path $env:TEMP 'arkchimy_echo_test'
$csv = "C:\Users\정민혁\AppData\Local\Temp\claude\778e2d5ee5455d20\a38b23f1-72a3-4960-9b05-c5ad86036408\scratchpad\deadlock_runs.csv"
foreach ($round in 1..$Count) {
    foreach ($w in $Workers) {
        $t = [IO.File]::ReadAllText($cfg)
        $t = $t -creplace 'workerCnt = \d+;', "workerCnt = $w;"
        [IO.File]::WriteAllText($cfg, $t, $utf8)
        $name = "deadlock_${Build}_w${w}_r${round}"
        $dir = Join-Path $outRoot $name
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        $startedAt = Get-Date
        $console = & "$wt\tools\echo_test.ps1" -Name $name -RunSec $RunSec -ClientCfgs 'LoadClient/client_chat.cfg','LoadClient/client_chatNoRecv.cfg'
        $exitedLine = $console | Where-Object { $_ -like '=== server exited by Ctrl+C*' }
        $srv = @(Get-Content "$dir\server.txt" -Encoding Default)
        $err = @(Get-Content "$dir\server_err.txt" -Encoding Default)
        $marks = @($srv | Where-Object { $_ -like '*deadlock-test*' }).Count
        $stats = @($srv | Where-Object { $_ -like '`[stat`]*' })
        $done = @($srv | Where-Object { $_ -like '*종료 완료*' }).Count
        $over = ''
        if ($stats.Count -gt 0 -and $stats[-1] -match '넘침끊김=(\d+)') { $over = $Matches[1] }
        $sess = ''
        if ($stats.Count -gt 0 -and $stats[-1] -match '세션=(\d+)') { $sess = $Matches[1] }
        $closes = @()
        foreach ($c in 0, 1) {
            $last = Get-Content "$dir\client$c.txt" -Encoding Default | Where-Object { $_ -match 'unexpectedClose=(\d+)' } | Select-Object -Last 1
            if ($last -match 'unexpectedClose=(\d+)') { $closes += $Matches[1] } else { $closes += '?' }
        }
        $row = [pscustomobject]@{
            Build = $Build; Worker = $w; Round = $round; Marks = $marks; StatLines = $stats.Count
            Overflow = $over; LastSessions = $sess; Done = $done; Exited = ($exitedLine -replace '=== server exited by Ctrl\+C: ', '' -replace ' ===', '')
            NormalClose = $closes[0]; NoRecvClose = $closes[1]; ErrLines = $err.Count; Started = $startedAt.ToString('HH:mm:ss')
        }
        $row | Export-Csv -Path $csv -Append -NoTypeInformation -Encoding UTF8
        $row | Format-List | Out-String
        Get-Process networkLibProject, LoadClient, EchoClient -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
}
