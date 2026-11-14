# 조사로 찾은 회복약 - 인벤토리가 가득 찬 사람은 "버림" 알림, 여유 있는 사람은 받음 알림, 파티원 모두 같은 알림
# 1번 방(조사, 난이도 10)의 판정은 주사위라 12판을 돌려 두 경우가 모두 나오기를 기다린다(한 판 성공 확률 55%)
. "$PSScriptRoot\GameTestCommon.ps1"
New-TestAccount 'pf_full' 100 @(1..32 | ForEach-Object { , @((924000 + $_), 3) })
New-TestAccount 'pf_room' 100 @()

Start-GameServer 'potion_full'
$full = New-GameClient 'full'; Say $full 'login pf_full' 600
$room = New-GameClient 'room'; Say $room 'login pf_room' 600
for ($try = 1; $try -le 12; ++$try)
{
	Say $full 'party create' 400
	Say $room "party join $try" 400
	Say $full 'party start' 600
	Say $full 'search' 400 # 차례는 방장(pf_full) 먼저
	Say $room 'search' 400
	Say $full 'party leave' 300
	Say $room 'party leave' 300
}
Say $full 'inv' 400
$outF = Close-GameClient $full
$outR = Close-GameClient $room
Stop-GameServer | Out-Null

$fF = @($outF | Where-Object { $_ -match '회복약을 찾' })
$fR = @($outR | Where-Object { $_ -match '회복약을 찾' })
$discard = @($fF | Where-Object { $_ -match 'pf_full 회복약을 찾았지만 인벤토리가 가득 차 버렸습니다' }).Count
$got = @($fF | Where-Object { $_ -match 'pf_room 회복약을 찾았습니다' }).Count
"===== 판정"
Test-Check '가득 찬 사람에게 버림 알림' ($discard -gt 0) "${discard}회"
Test-Check '여유 있는 사람에게 받음 알림' ($got -gt 0) "${got}회"
Test-Check '두 파티원이 같은 알림을 같은 순서로' ((($fF -join "`n") -ceq ($fR -join "`n")))
Test-Check '가득 찬 계정은 아이템 32개 그대로' ($outF -match '아이템 32개')
Test-Check 'DB: 가득 찬 계정의 획득 로그 없음, 받은 계정은 받은 만큼' (("$(Invoke-GameSql "SELECT COUNT(*) FROM logs WHERE account_id = 'pf_full' AND kind = 'potion_found'")".Trim() -eq '0') -and ("$(Invoke-GameSql "SELECT COUNT(*) FROM logs WHERE account_id = 'pf_room' AND kind = 'potion_found'")".Trim() -eq "$got"))
Write-Verdict 'potion_full'
