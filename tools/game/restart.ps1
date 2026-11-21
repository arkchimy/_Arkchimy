# 재시작 - 처음 입장한 아이디의 계정 행이 생기는지, 차단이 DB에 남아 다시 켠 서버에서도 막는지(메모리가 아니라 DB에서 읽음)
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss'
$id = "rs$tag" # 매번 처음 보는 아이디 - 계정 행은 서버가 첫 입장 때 만든다
New-TestCredential $id

Start-GameServer 'restart_1'
$c = New-GameClient 'c'; Say $c "login $id" 800
$a = New-AdminTool; Start-Sleep 1
Say $a "ban $id 10" 800
$outA1 = Close-GameClient $a
$out1 = Close-GameClient $c
Stop-GameServer | Out-Null
$rowCnt = "$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE account_id = '$id'")".Trim()

Start-GameServer 'restart_2'
$c = New-GameClient 'c'; Say $c "login $id" 800
$out2 = Close-GameClient $c
$a = New-AdminTool; Start-Sleep 1
Say $a "unban $id" 800
$outA2 = Close-GameClient $a
$d = New-GameClient 'd'; Say $d "login $id" 800
$out3 = Close-GameClient $d
Stop-GameServer | Out-Null

"===== 판정"
Test-Check '1회차: 처음 보는 아이디로 입장 성공' ($out1 -match '\[로그인\] 성공')
Test-Check '1회차: 계정 행이 하나 생김' ($rowCnt -eq '1') "${rowCnt}줄"
Test-Check '1회차: 접속 중 차단 → 끊김' (($outA1 -match "$id 적용함 - 접속 중이라 끊음") -and ($out1 -match '연결 끊김'))
Test-Check '2회차: 다시 켠 서버가 DB의 차단을 읽어 입장 실패' ($out2 -match '입장 실패')
Test-Check '2회차: 해제(DB에만 있던 계정을 메모리에 올린 뒤) → 입장 성공' (($outA2 -match "$id 적용함") -and ($out3 -match '\[로그인\] 성공'))
Test-Check '계정 행은 그대로 하나' ("$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE account_id = '$id'")".Trim() -eq '1')
Write-Verdict 'restart'
