# 운영 툴 - 차단·해제(DB에만 있는 계정 / 없는 계정 / 접속 중인 계정), 인증 기한, 분 입력 검증
. "$PSScriptRoot\GameTestCommon.ps1"
New-TestAccount 'ad_online'
New-TestAccount 'ad_offline'

Start-GameServer 'admin'
$listen = Get-NetTCPConnection -State Listen -LocalPort 33001 -ErrorAction SilentlyContinue | Select-Object -First 1
# 키를 보내지 않는 연결 4개 - 5초 뒤 서버가 끊어야 운영 툴이 들어온다
$idle = 1..4 | ForEach-Object { $t = New-Object Net.Sockets.TcpClient; $t.Connect('127.0.0.1', 33001); $t }
Start-Sleep 7
$closed = 0
foreach ($t in $idle)
{
	$t.Client.ReceiveTimeout = 500
	try { if ($t.GetStream().Read((New-Object byte[] 8), 0, 8) -eq 0) { ++$closed } } catch { ++$closed }
	$t.Close()
}

$o = New-GameClient 'o'; Say $o 'login ad_online' 600
$a = New-AdminTool; Start-Sleep 1
Say $a 'ban ad_offline' 400       # 분 없음 - 거절(해제로 보내지 않는다)
Say $a 'ban ad_offline 1h' 400    # 숫자 아님 - 거절
Say $a 'ban ad_offline 5' 600     # DB에만 있는 계정
Say $a 'ban ghost_none 5' 600     # 없는 계정
Say $a 'ban ad_online 10' 800     # 접속 중 - 끊김
$o2 = New-GameClient 'o2'; Say $o2 'login ad_online' 800 # 차단 중 - 입장 실패(응답 없이 끊김)
$f = New-GameClient 'f'; Say $f 'login ad_offline' 800   # DB에서 읽은 차단 - 입장 실패
Say $a 'unban ad_online' 600
$o3 = New-GameClient 'o3'; Say $o3 'login ad_online' 800 # 입장에 실패한 연결은 끊기므로 새 클라로
$outA = Close-GameClient $a
$out = @{}
foreach ($p in $o, $o2, $f, $o3) { $out[$p.Tag] = Close-GameClient $p }
Stop-GameServer | Out-Null

"===== 판정"
Test-Check '운영자 포트는 127.0.0.1에만' ($listen -and $listen.LocalAddress -eq '127.0.0.1') "$($listen.LocalAddress)"
Test-Check '키 없는 연결 4개가 5초 뒤 끊김' ($closed -eq 4) "$closed/4"
Test-Check '분 입력 검증(빈 값·1h 거절)' (@($outA | Where-Object { $_ -match '분은 1 이상의 숫자' }).Count -eq 2)
Test-Check '제재: DB만·없는 계정·접속 중(끊김)' (($outA -match 'ad_offline 적용함') -and ($outA -match 'ghost_none 그런 계정이 없음') -and ($outA -match 'ad_online 적용함 - 접속 중이라 끊음') -and ($out['o'] -match '연결 끊김'))
Test-Check '차단 중 입장 실패(메모리 계정·DB에서 읽은 계정), 해제 뒤 로그인 성공' (($out['o2'] -match '입장 실패') -and ($out['f'] -match '입장 실패') -and ($out['o3'] -match '\[로그인\] 성공'))
Test-Check 'DB: ad_offline 차단 시각이 저장됨, ad_online은 해제됨' (("$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE account_id = 'ad_offline' AND ban_until > NOW()")".Trim() -eq '1') -and ("$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE account_id = 'ad_online' AND ban_until IS NULL")".Trim() -eq '1'))
Test-Check '서버: 입장거절 2(차단 둘), 입장확인실패 0' ((Get-ServerStat '입장거절') -eq 2 -and (Get-ServerStat '입장확인실패') -eq 0) "입장거절 $(Get-ServerStat '입장거절'), 입장확인실패 $(Get-ServerStat '입장확인실패')"
Write-Verdict 'admin'
