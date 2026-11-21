# DB 연결 끊김 - 서버가 도는 중에 DB 쪽에서 서버 연결을 KILL → 다음 DB 일(처음 보는 아이디의 입장 = 계정 행 읽기·만들기)에서 끊김 감지, 다시 연결
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss'
$id = "rc$tag" # 처음 보는 아이디 - 입장하면 DB 스레드가 계정 행을 읽고 만든다
New-TestCredential $id

Start-GameServer 'reconnect'
$ids = Invoke-GameSql "SELECT id FROM information_schema.processlist WHERE user = (SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1)) AND id <> CONNECTION_ID() AND db = DATABASE()"
foreach ($i in $ids) { Invoke-GameSql "KILL $i" | Out-Null }
$c = New-GameClient 'c'; Say $c "login $id" 1200 # 끊긴 연결로 읽기 → 2013 → 다시 연결 → 다시 읽기 → 만들기
Say $c 'chat after-reconnect' 400
$out = Close-GameClient $c
Stop-GameServer | Out-Null
$err = Get-Content "$GameOut\server_err.txt" -Encoding Default

"===== 판정"
Test-Check '서버 쪽 DB 연결을 끊음' (@($ids).Count -gt 0) "끊은 연결 $(@($ids) -join ',')"
Test-Check '입장 성공(채팅이 돌아옴)' (($out -match '\[로그인\] 성공') -and ($out -match "\[채팅\] ${id}: after-reconnect"))
Test-Check '끊김 감지 후 다시 연결' (($err -match 'err=2013|err=2006') -and ($err -match '다시 연결함'))
Test-Check 'DB에 계정 행이 생김' ("$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE account_id = '$id'")".Trim() -eq '1')
Test-Check 'DB 실패 0' ((Get-ServerStat 'DB실패') -eq 0)
Write-Verdict 'reconnect'
