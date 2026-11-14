# 재시작 - 장비 착용이 남는지, 끌 때 열려 있던 경매가 다음 시작 때 취소·환불되는지(GameDB::Prepare)
. "$PSScriptRoot\GameTestCommon.ps1"
New-TestAccount 'rs_seller' 100 @(@(921001, 3), @(921002, 2))
New-TestAccount 'rs_bidder' 100 @()

Start-GameServer 'restart_1'
$c = New-GameClient 'c'; Say $c 'login rs_seller' 500
$b = New-GameClient 'b'; Say $b 'login rs_bidder' 500
Say $c 'equip 921001' 300
Say $c 'auction sell 921002 30' 300
$auction = Get-OpenAuctionID 'rs_seller'
Say $b "auction bid $auction 30" 300
Say $b 'inv' 300
$out1b = Close-GameClient $b
$out1c = Close-GameClient $c
Stop-GameServer | Out-Null # 경매(60초)가 끝나기 전에 끈다

Start-GameServer 'restart_2'
$prepare = Get-Content "$GameOut\server.txt" -Encoding Default | Where-Object { $_ -match '열려 있던 경매' }
$c = New-GameClient 'c'; Say $c 'login rs_seller' 500
$b = New-GameClient 'b'; Say $b 'login rs_bidder' 500
Say $c 'inv' 300
Say $b 'inv' 300
$out2c = Close-GameClient $c
$out2b = Close-GameClient $b
Stop-GameServer | Out-Null

"===== 판정"
Test-Check '1회차: 입찰로 골드 70' ($out1b -match '골드 70,')
Test-Check '재시작 때 열린 경매 정리 로그' ($prepare.Count -gt 0) "$prepare"
Test-Check '2회차: 입찰자 골드 100으로 환불' ($out2b -match '골드 100,')
Test-Check '2회차: 경매에 올렸던 아이템이 판매자에게 돌아옴' ($out2c -match '#921002')
Test-Check '2회차: 장비 착용이 남음' ($out2c -match '#921001 .*\(착용')
Test-Check 'DB: 그 경매는 cancelled' (("$(Invoke-GameSql "SELECT state FROM auctions WHERE auction_id = $auction")".Trim()) -eq 'cancelled')
Test-Check 'DB 골드 = 로그 합계(이 시험의 계정)' ((Test-GoldMatchesLog 'rs[_]%') -eq 0)
Write-Verdict 'restart'
