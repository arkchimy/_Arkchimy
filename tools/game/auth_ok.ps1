# 인증 분산 - 정상 경로만: 가입 → 로그인 서버에서 세션 키(Redis) → 게임 서버 입장(키 확인·지우기, 계정 행) → 끊기
# 클라는 입장에 성공하면 바로 끊는다. 처음 보는 아이디(계정 행을 만든다)와 같은 아이디의 두 번째 입장(계정 행을 읽는다)을 본다
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss'
$ids = 1..3 | ForEach-Object { "ok$tag$_" } # 매번 처음 보는 아이디
Start-GameServer 'auth_ok'

$first = @{}
foreach ($id in $ids)
{
	$c = New-GameClient "f_$id"
	Say $c "register $id" 400
	Say $c "login $id" 800
	$first[$id] = Close-GameClient $c # 입장 확인 뒤 바로 끊는다
}
$rowsAfterFirst = "$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE CAST(account_id AS CHAR) LIKE 'ok$tag%'")".Trim()
$second = @{}
foreach ($id in $ids)
{
	$c = New-GameClient "s_$id"
	Say $c "login $id" 800
	$second[$id] = Close-GameClient $c
}
Start-Sleep -Milliseconds 1300 # 통계 줄은 1초마다
$redisKeys = "$(Invoke-Redis @('DBSIZE'))".Trim()
$rowsAfterSecond = "$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE CAST(account_id AS CHAR) LIKE 'ok$tag%'")".Trim()
Stop-GameServer | Out-Null

"===== 판정"
Test-Check '가입 3명 성공' (@($ids | Where-Object { $first[$_] -match '\[가입\] 가입했습니다' }).Count -eq 3)
Test-Check '첫 입장 3명 성공(처음 보는 아이디)' (@($ids | Where-Object { $first[$_] -match '\[로그인\] 성공' }).Count -eq 3)
Test-Check '첫 입장 뒤 계정 행 3줄' ($rowsAfterFirst -eq '3') "${rowsAfterFirst}줄"
Test-Check '두 번째 입장 3명 성공(같은 아이디, 새 세션 키)' (@($ids | Where-Object { $second[$_] -match '\[로그인\] 성공' }).Count -eq 3)
Test-Check '두 번째 입장 뒤에도 계정 행 3줄(새로 만들지 않음)' ($rowsAfterSecond -eq '3') "${rowsAfterSecond}줄"
Test-Check '어떤 클라도 서버가 먼저 끊지 않음' (@($ids | Where-Object { ($first[$_] + $second[$_]) -match '\[연결 끊김\]|입장 실패' }).Count -eq 0)
Test-Check 'Redis에 남은 세션 키 0(입장 때 지움)' ($redisKeys -eq '0') "DBSIZE $redisKeys"
$ok = Get-LoginStat '로그인성공'; $ack = Get-LoginStat '확인받음'
Test-Check '로그인 서버: 로그인 성공 6 = 확인(AuthAck) 받음 6, 확인시간초과 0' ($ok -eq 6 -and $ack -eq 6 -and (Get-LoginStat '확인시간초과') -eq 0) "로그인성공 $ok, 확인받음 $ack"
Test-Check '게임 서버: 입장확인실패·입장거절·잘못된패킷 0' ((Get-ServerStat '입장확인실패') -eq 0 -and (Get-ServerStat '입장거절') -eq 0 -and (Get-ServerStat '잘못된패킷') -eq 0)
Test-Check '두 서버 세션 0' ((Get-ServerStat '세션') -eq 0 -and (Get-LoginStat '세션') -eq 0)
Write-Verdict 'auth_ok'
