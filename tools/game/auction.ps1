# 경매장 시나리오 - 등록·거절 경우·입찰(미리 차감, 밀리면 환불)·낙찰·유찰, 구경꾼 알림 순서, DB 골드 = 로그 합계
# 계정은 DB에 직접 만든다(던전 결과에 기대지 않게). 경매 기간을 server.cfg에서 잠시 5초로
. "$PSScriptRoot\GameTestCommon.ps1"
New-TestAccount 'at_seller' 100 @(, @(920001, 1))
New-TestAccount 'at_bidb' 100 @()
New-TestAccount 'at_bidc' 100 @()
New-TestAccount 'at_nobody' 100 @(, @(920002, 1))
$orig = Set-ServerCfg 'auctionDurationSec' 5
try
{
	Start-GameServer 'auction'
	$a = New-GameClient 'a'; Say $a 'login at_seller' 500
	$b = New-GameClient 'b'; Say $b 'login at_bidb' 500
	$c = New-GameClient 'c'; Say $c 'login at_bidc' 500
	$d = New-GameClient 'd'; Say $d 'login at_nobody' 500
	Say $c 'auction watch on'; Say $d 'auction watch on'

	# 던전 중 등록 거절 - b·c 파티로 던전을 시작해 두고 b가 등록
	Say $b 'party create' 500
	Say $c 'party join 1' 500 # 파티 번호는 메모리에만 있다 - 서버를 새로 켰으므로 1번
	Say $b 'party start' 600
	Say $b 'auction sell 1 10' 300
	Say $b 'party leave'; Say $c 'party leave'

	Say $a 'auction sell 999 10'          # 없는 아이템
	Say $a 'auction sell 920001 0'        # 시작가 0
	Say $a 'auction sell 920001 10' 300
	$sold = Get-OpenAuctionID 'at_seller'
	Say $d 'auction sell 920002 7' 300
	$unsold = Get-OpenAuctionID 'at_nobody'
	Say $a "auction bid $sold 10"         # 내 경매
	Say $b "auction bid $sold 5"          # 시작가 미만
	Say $b "auction bid $sold 9999"       # 골드 모자람
	Say $b "auction bid $sold 10"         # 첫 입찰
	Say $c "auction bid $sold 10"         # 같은 값 - 5%(최소 1) 미만
	Say $c "auction bid $sold 20"         # b 밀림·환불
	Say $c "auction bid $sold 30"         # 이미 최고 입찰자
	Say $b "auction bid $sold 21" 20      # 20 + 1 이상이면 된다 - c 밀림
	Say $b "auction bid $unsold 7"        # 100ms 안 - 너무 빠름
	Start-Sleep 6
	foreach ($p in $a, $b, $c, $d) { Say $p 'inv' 300 }
	$out = @{}
	foreach ($p in $a, $b, $c, $d) { $out[$p.Tag] = Close-GameClient $p }
	Stop-GameServer
}
finally
{
	Restore-ServerCfg $orig
}

"===== 판정"
Test-Check '던전 중 등록 거절' ($out['b'] -match '던전 중에는 등록할 수 없음')
Test-Check '없는 아이템·시작가 0 거절' (($out['a'] -match '인벤토리에 없는 아이템') -and ($out['a'] -match '시작가는 1'))
Test-Check '내 경매·낮은 값·골드 부족·최고 입찰자·너무 빠름 거절' (($out['a'] -match '내가 올린 경매') -and ($out['b'] -match '가격이 낮음') -and ($out['b'] -match '골드가 모자람') -and ($out['c'] -match '이미 최고 입찰자') -and ($out['b'] -match '너무 빨리'))
Test-Check '밀린 입찰자 환불 알림' (($out['b'] -match '밀림 - 골드 10 돌려받음') -and ($out['c'] -match '밀림 - 골드 20 돌려받음'))
Test-Check '낙찰·판매·유찰 알림' (($out['b'] -match '낙찰 - 21골드') -and ($out['a'] -match '판매 - 골드 \+21') -and ($out['d'] -match '팔리지 않음'))
Test-Check '최종 골드(판매자 121, 낙찰자 79, 밀린 사람 100, 유찰 100)' ((($out['a'] -match '골드 121') -and ($out['b'] -match '골드 79') -and ($out['c'] -match '골드 100,') -and ($out['d'] -match '골드 100,')))
$watchC = @($out['c'] | Where-Object { $_ -match '^\[경매장\]' }) -join "`n"
$watchD = @($out['d'] | Where-Object { $_ -match '^\[경매장\]' }) -join "`n"
Test-Check '구경꾼 둘이 같은 알림을 같은 순서로' (($watchC -ceq $watchD) -and $watchC.Length -gt 0)
Test-Check '아이템 위치(DB): 낙찰품은 낙찰자, 유찰품은 판매자' ((("$(Invoke-GameSql "SELECT CAST(account_id AS CHAR) FROM items WHERE item_uid = 920001")".Trim()) -eq 'at_bidb') -and (("$(Invoke-GameSql "SELECT CAST(account_id AS CHAR) FROM items WHERE item_uid = 920002 AND auction_id IS NULL")".Trim()) -eq 'at_nobody'))
Test-Check 'DB 골드 = 로그 합계(이 시험의 계정)' ((Test-GoldMatchesLog 'at[_]%') -eq 0)
Test-Check 'DB 실패 0' ((Get-ServerStat 'DB실패') -eq 0)
Write-Verdict 'auction'
