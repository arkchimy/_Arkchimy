# 로그인 서버와 게임 서버 입장 확인(설계 login_server.docx 9-2) - 가입·중복 가입·틀린 비밀번호·한 세션 5번 실패 끊기·
# 세션 키 재사용 거절·가짜 첫 메시지로 키가 지워지지 않음·30초 만료·Redis에 키가 남지 않음
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss' # 매번 처음 보는 아이디
$id = "lg$tag"
Start-GameServer 'login'

# 1. 가입·로그인 서버 결과
$u = New-GameClient 'u'
Say $u "register $id" 400
Say $u "register $id" 400
Say $u "register ${id}b 1234" 200
Say $u "login $id wrong-password" 400
Say $u "login nouser$tag" 400
Say $u "login $id" 600
$uOut = Close-GameClient $u

# 2. 한 연결에서 5번 틀리면 로그인 서버가 끊는다 - 클라는 요청마다 새로 접속하므로 소켓을 직접 쓴다
function New-AuthPacket($accountID, $password)
{
	$body = New-Object byte[] (2 + 16 + 65)
	[BitConverter]::GetBytes([uint16]3).CopyTo($body, 0) # AuthReq
	[Text.Encoding]::ASCII.GetBytes($accountID).CopyTo($body, 2)
	[Text.Encoding]::ASCII.GetBytes($password).CopyTo($body, 18)
	return [byte[]]([BitConverter]::GetBytes([uint16]$body.Length) + $body)
}
$tcp = New-Object Net.Sockets.TcpClient('127.0.0.1', 33002)
$stream = $tcp.GetStream()
$stream.ReadTimeout = 5000
$answered = 0
$closedAfter = -1
for ($i = 1; $i -le 6; ++$i)
{
	try
	{
		$pkt = New-AuthPacket $id 'wrong-password'
		$stream.Write($pkt, 0, $pkt.Length)
		$buf = New-Object byte[] 64
		$n = $stream.Read($buf, 0, $buf.Length)
		if ($n -le 0) { $closedAfter = $i - 1; break }
		++$answered
	}
	catch { $closedAfter = $i - 1; break }
}
$tcp.Close()

# 3. 세션 키 재사용·가짜 첫 메시지
$a = New-GameClient 'a'; Say $a "auth $id" 500
$aOut = Close-GameClient $a
$keyLine = $aOut | Where-Object { $_ -match '\[세션 키\] no=(\d+) key=([0-9a-f]{64})' } | Select-Object -First 1
$no = $Matches[1]; $key = $Matches[2]
$fake = New-GameClient 'fake'; Say $fake "enter $no $key bad" 500
$fakeOut = Close-GameClient $fake
$real = New-GameClient 'real'; Say $real "enter $no $key" 600
$realOut = Close-GameClient $real
$again = New-GameClient 'again'; Say $again "enter $no $key" 500
$againOut = Close-GameClient $again

# 4. 30초 만료
$e = New-GameClient 'e'; Say $e "auth $id" 500
$eOut = Close-GameClient $e
$null = $eOut | Where-Object { $_ -match '\[세션 키\] no=(\d+) key=([0-9a-f]{64})' } | Select-Object -First 1
$eNo = $Matches[1]; $eKey = $Matches[2]
Start-Sleep 31
$late = New-GameClient 'late'; Say $late "enter $eNo $eKey" 500
$lateOut = Close-GameClient $late
Start-Sleep 2
$redisKeys = "$(Invoke-Redis @('DBSIZE'))".Trim()

# 5. Redis가 꺼지면 로그인은 오래 기다리지 않고 실패한다(연결 1초 제한, 실패 뒤 5초는 바로 실패) - 인증·DB 스레드가 붙잡히지 않게
Stop-Redis
$watch = [Diagnostics.Stopwatch]::StartNew()
$down = New-GameClient 'down'
Say $down "login $id" 100
Say $down "login $id" 100
Say $down "login $id" 100
$downOut = Close-GameClient $down
$watch.Stop()
Stop-GameServer

"===== 판정"
Test-Check '가입 성공' ($uOut -match '\[가입\] 가입했습니다')
Test-Check '같은 아이디 다시 가입 → 이미 있음' ($uOut -match '\[가입\] 이미 있는 아이디')
Test-Check '짧은 비밀번호 → 거절' ($uOut -match '\[가입\] 비밀번호는 8~64바이트')
Test-Check '틀린 비밀번호·없는 아이디 → 같은 실패' (@($uOut -match '\[로그인 서버\] 아이디 또는 비밀번호가 틀립니다').Count -eq 2)
Test-Check '맞는 비밀번호 → 게임 서버 입장' ($uOut -match '\[로그인\] 성공')
Test-Check '한 연결 5번 실패 뒤 끊김' ($closedAfter -eq 5 -or ($closedAfter -eq 4 -and $answered -eq 4)) "응답 ${answered}번, 끊긴 시점: ${closedAfter}번째 요청 뒤"
Test-Check '세션 키 받음' ($null -ne $keyLine)
Test-Check '틀린 증명 → 입장 실패' ($fakeOut -match '\[로그인\] 입장 확인 실패')
Test-Check '틀린 증명 뒤에도 진짜 키로 입장(키가 지워지지 않음)' ($realOut -match '\[로그인\] 성공')
Test-Check '같은 키 다시 → 입장 실패(한 번만)' ($againOut -match '\[로그인\] 입장 확인 실패')
Test-Check '30초 지난 키 → 입장 실패' ($lateOut -match '\[로그인\] 입장 확인 실패')
Test-Check 'Redis에 남은 키 0' ($redisKeys -eq '0') "DBSIZE $redisKeys"
Test-Check 'Redis가 꺼지면 로그인 3번 모두 서버 오류' (@($downOut -match '\[로그인 서버\] 서버 오류').Count -eq 3)
Test-Check 'Redis가 꺼져도 오래 기다리지 않음(3번에 5초 안)' ($watch.Elapsed.TotalSeconds -lt 5) "$([math]::Round($watch.Elapsed.TotalSeconds, 1))초"
Test-Check '게임 서버 세션 0' ((Get-ServerStat '세션') -eq 0)
Test-Check '로그인 서버 세션 0' ((Get-LoginStat '세션') -eq 0)
Write-Verdict 'login'
