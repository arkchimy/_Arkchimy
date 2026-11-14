# 4명 던전 - 시작 3초 뒤 차례가 아닌 파티원이 나가면 같은 턴 알림이 남은 시간으로 다시 오고, 남은 파티원은 끝까지 같은 이벤트를 같은 순서로 받는다
# 던전 결과(성공·실패)는 주사위라 매번 다르다 - 결과가 아니라 순서와 시간을 본다
. "$PSScriptRoot\GameTestCommon.ps1"
Start-GameServer 'dungeon4'
$tag = Get-Date -Format 'HHmmss'
$p = 1..4 | ForEach-Object { $c = New-GameClient "p$_"; Say $c "login d4${tag}_$_" 500; $c }
Say $p[0] 'party create' 500
foreach ($c in $p[1..3]) { Say $c 'party join 1' 400 }
Say $p[0] 'party start' 600
Start-Sleep 3
Say $p[3] 'party leave' 600 # 첫 차례는 p1 - p4는 차례가 아니다
Invoke-DungeonSpam $p[0..2] 80
Start-Sleep 1
$out = @{}
foreach ($c in $p) { $out[$c.Tag] = Close-GameClient $c }
Stop-GameServer | Out-Null

$turns = @($out['p1'] | Where-Object { $_ -match '^\[던전 1번 방, 턴 1\]' })
$seq = @{}
foreach ($k in 'p1', 'p2', 'p3') { $seq[$k] = @($out[$k] | Where-Object { $_ -match '^\[던전' -and $_ -notmatch '할 수 없는 행동|던전 시작' }) -join "`n" }
"===== 판정"
Test-Check '첫 턴 알림 10초, 파티원이 빠진 뒤 같은 턴이 남은 시간(10초 미만)으로' (($turns.Count -ge 2) -and ($turns[0] -match '\(10초\)') -and ($turns[1] -match '\(([0-9])초\)')) "$($turns[1])"
Test-Check '던전이 끝남(성공 또는 실패)' ($out['p1'] -match '\[던전 끝\]')
Test-Check '남은 세 명의 던전 이벤트가 같은 순서' (($seq['p1'] -ceq $seq['p2']) -and ($seq['p1'] -ceq $seq['p3'])) "이벤트 $(($seq['p1'] -split "`n").Count)줄"
Test-Check '나간 파티원은 파티 없음 알림' ($out['p4'] -match '파티에서 나왔습니다')
Test-Check 'DB 실패 0' ((Get-ServerStat 'DB실패') -eq 0)
Write-Verdict 'dungeon4'
