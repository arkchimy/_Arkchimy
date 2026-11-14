# 운영 툴 - 회수 세 경로(경매 중 / 접속한 계정의 착용 장비 / 로그인 안 한 계정)와 없는 아이템, 제재·해제, 인증 기한, 분 입력 검증, 한글 사유
. "$PSScriptRoot\GameTestCommon.ps1"
New-TestAccount 'ad_seller' 100 @(, @(923001, 1))
New-TestAccount 'ad_bidder' 100 @()
New-TestAccount 'ad_online' 100 @(, @(923002, 3))
New-TestAccount 'ad_offline' 100 @(, @(923003, 1))

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

$s = New-GameClient 's'; Say $s 'login ad_seller' 500
$b = New-GameClient 'b'; Say $b 'login ad_bidder' 500
$o = New-GameClient 'o'; Say $o 'login ad_online' 500
Say $o 'equip 923002' 300
Say $s 'auction sell 923001 10' 300
$auction = Get-OpenAuctionID 'ad_seller'
Say $b "auction bid $auction 10" 300

$a = New-AdminTool; Start-Sleep 1
Say $a 'recall 923001 경매 중 회수' 600
Say $a 'recall 923002 버그로 얻은 투구' 600
Say $a 'recall 923003 테스트 회수' 600
Say $a 'recall 999999999' 600
Say $a 'ban ad_offline' 400       # 분 없음 - 거절(해제로 보내지 않는다)
Say $a 'ban ad_offline 1h' 400    # 숫자 아님 - 거절
Say $a 'ban ad_offline 5' 600     # DB에만 있는 계정
Say $a 'ban ghost_none 5' 600     # 없는 계정
Say $a 'ban ad_online 10 악용' 800 # 접속 중 - 끊김
$o2 = New-GameClient 'o2'; Say $o2 'login ad_online' 800
Say $a 'unban ad_online 해제' 600
Say $o2 'login ad_online' 800
Say $a 'item 923002' 600
$outA = Close-GameClient $a
$out = @{}
foreach ($p in $s, $b, $o, $o2) { $out[$p.Tag] = Close-GameClient $p }
Stop-GameServer | Out-Null

"===== 판정"
Test-Check '운영자 포트는 127.0.0.1에만' ($listen -and $listen.LocalAddress -eq '127.0.0.1') "$($listen.LocalAddress)"
Test-Check '키 없는 연결 4개가 5초 뒤 끊김' ($closed -eq 4) "$closed/4"
Test-Check '회수 ① 경매 중 - 경매 취소, 입찰자 환불' (($outA -match '회수함 .*#923001.*경매') -and ($out['b'] -match '거래 취소.*골드 10 돌려받음') -and ($out['s'] -match '운영자가 회수'))
Test-Check '회수 ② 접속한 계정의 착용 장비' (($outA -match '회수함 .*#923002.*인벤토리') -and ($out['o'] -match '\[운영자\].*#923002'))
Test-Check '회수 ③ 로그인 안 한 계정(DB만)' ($outA -match '회수함 .*#923003.*DB')
Test-Check '없는 아이템' ($outA -match '그런 아이템이 없음')
Test-Check '분 입력 검증(빈 값·1h 거절)' (@($outA | Where-Object { $_ -match '분은 1 이상의 숫자' }).Count -eq 2)
Test-Check '제재: DB만·없는 계정·접속 중(끊김)' (($outA -match 'ad_offline 적용함') -and ($outA -match 'ghost_none 그런 계정이 없음') -and ($outA -match 'ad_online 적용함 - 접속 중이라 끊음') -and ($out['o'] -match '연결 끊김'))
Test-Check '차단 중 로그인 거절, 해제 뒤 로그인 성공' (($out['o2'] -match '로그인을 막은 아이디') -and ($out['o2'] -match '\[로그인\] 성공'))
Test-Check '아이템 이력에 한글 사유' ($outA -match 'recall .*버그로 얻은 투구')
Test-Check 'DB: 회수한 아이템 셋이 없음' (("$(Invoke-GameSql "SELECT COUNT(*) FROM items WHERE item_uid IN (923001, 923002, 923003)")".Trim()) -eq '0')
Test-Check 'DB 골드 = 로그 합계(이 시험의 계정)' ((Test-GoldMatchesLog 'ad[_]%') -eq 0)
Write-Verdict 'admin'
