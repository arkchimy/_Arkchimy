# 로그인 중 이중 로그인 - 처음 보는 아이디로 로그인한 직후(DB 스레드가 읽는 중) 다른 아이디로 또 로그인 → 이 세션은 잘못된 패킷으로 끊기고,
# 첫 아이디는 잠기지 않아 새 접속으로 다시 로그인할 수 있어야 한다(로그인 중 표시를 계정보다 먼저 읽고, 표시를 내린 뒤 응답)
# 경쟁 창은 나노초 수준이라 이 시험이 그 순간을 재현하는 것은 아니다 - 고친 순서가 정상 경로를 깨지 않는지와 잠김이 없는지를 본다
. "$PSScriptRoot\GameTestCommon.ps1"
$round = 20
$tag = Get-Date -Format 'HHmmss' # 매번 처음 보는 아이디가 되게
Start-GameServer 'login_race'
$ok = 0
for ($i = 0; $i -lt $round; ++$i)
{
	$c = New-GameClient "c$i"
	$c.StandardInput.WriteLine("login lr${tag}_$i")
	$c.StandardInput.WriteLine("login lr${tag}_x$i")
	$c.StandardInput.Flush()
	Start-Sleep -Milliseconds 400
	Close-GameClient $c | Out-Null
	Start-Sleep -Milliseconds 200
	$d = New-GameClient "d$i"
	Say $d "login lr${tag}_$i" 500
	if ((Close-GameClient $d) -match '\[로그인\] 성공') { ++$ok }
}
Stop-GameServer | Out-Null

"===== 판정"
Test-Check '이중 로그인 세션은 모두 잘못된 패킷으로 끊김' ((Get-ServerStat '잘못된패킷') -eq $round) "$(Get-ServerStat '잘못된패킷')/$round"
Test-Check '첫 아이디로 다시 로그인 성공(잠김 없음)' ($ok -eq $round) "$ok/$round"
Test-Check '세션 0으로 돌아옴' ((Get-ServerStat '세션') -eq 0)
Write-Verdict 'login_race'
