# DB 연결 끊김 - 서버가 도는 중에 DB 쪽에서 서버 연결을 KILL → 다음 저장에서 끊김 감지, 다시 연결, 같은 트랜잭션 재실행
. "$PSScriptRoot\GameTestCommon.ps1"
New-TestAccount 'rc_user' 100 @(, @(922001, 3))

Start-GameServer 'reconnect'
$c = New-GameClient 'c'; Say $c 'login rc_user' 600
$ids = Invoke-GameSql "SELECT id FROM information_schema.processlist WHERE user = (SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1)) AND id <> CONNECTION_ID() AND db = DATABASE()"
foreach ($i in $ids) { Invoke-GameSql "KILL $i" | Out-Null }
Say $c 'equip 922001' 800 # 끊긴 연결로 저장 → 2013 → 다시 연결 → 재실행
$out = Close-GameClient $c
Stop-GameServer | Out-Null
$err = Get-Content "$GameOut\server_err.txt" -Encoding Default

"===== 판정"
Test-Check '서버 쪽 DB 연결을 끊음' (@($ids).Count -gt 0) "끊은 연결 $(@($ids) -join ',')"
Test-Check '장비 착용 성공' ($out -match '\[장비\] 성공')
Test-Check '끊김 감지 후 다시 연결' (($err -match 'err=2013|err=2006') -and ($err -match '다시 연결함'))
Test-Check 'DB에 착용이 저장됨(재실행)' (("$(Invoke-GameSql "SELECT IFNULL(equip_slot, -1) FROM items WHERE item_uid = 922001")".Trim()) -ne '-1')
Test-Check 'DB 실패 0' ((Get-ServerStat 'DB실패') -eq 0)
Write-Verdict 'reconnect'
