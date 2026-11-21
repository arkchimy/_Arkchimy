# 게임 서버 ↔ 클라 입장 비정상 시험 - 테스트 케이스 portfolio/verification/게임서버_입장_비정상_테스트케이스.docx 3판(H·E·R·S·M 26개)
# 구조: ① Hello [type | AccountNo] 평문 → ② Challenge [type | SeqNumber 시작값] 평문 → ③ 입장 요청부터 통일 모양 [type | SeqNumber] + 암호문 + 태그
# 잘못된 바이트는 이 스크립트가 소켓으로 직접 보내고, 암호가 필요한 입장 요청은 클라 시험 명령 enterdump로 만든다(받은 시작값을 넣어)
# 판정은 게임 서버 통계의 증가분(입장확인실패·입장거절·잘못된패킷·입장기한끊김)과 Redis 키로 한다 - 응답 없이 끊으므로 클라는 이유를 모른다
# 시험 동안 server.cfg entryLimitSec를 5초로 줄인다(H-01·H-02). 끝나면 되돌린다
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss'
$users = 'ea', 'eb', 'ec', 'ed', 'ee' | ForEach-Object { "$_$tag" }
foreach ($u in $users) { New-TestCredential $u }

# ---------------------------------------------------------------- 도구
function New-Raw
{
	$t = New-Object Net.Sockets.TcpClient
	$t.NoDelay = $true
	$t.Connect('127.0.0.1', 33000)
	return $t
}

function Send-Bytes($t, [byte[]]$bytes) { $t.GetStream().Write($bytes, 0, $bytes.Length) }

# payload 앞에 라이브러리 헤더(길이 2바이트)를 붙여 보낸다
function Send-Payload($t, [byte[]]$payload) { Send-Bytes $t ([byte[]]([BitConverter]::GetBytes([uint16]$payload.Length) + $payload)) }

function New-HelloBytes([uint64]$no) { [byte[]](@(1, 0) + [BitConverter]::GetBytes($no)) }

# 받을 것을 기다린다 - State: data(Bytes) / closed(상대가 끊음) / timeout
function Read-Some($t, $ms = 1500)
{
	$t.Client.ReceiveTimeout = $ms
	$buf = New-Object byte[] 4096
	try
	{
		$n = $t.GetStream().Read($buf, 0, $buf.Length)
		if ($n -le 0) { return [pscustomobject]@{ State = 'closed'; Bytes = [byte[]]@() } }
		return [pscustomobject]@{ State = 'data'; Bytes = [byte[]]$buf[0..($n - 1)] }
	}
	catch
	{
		$e = $_.Exception
		while ($e -and -not ($e -is [Net.Sockets.SocketException])) { $e = $e.InnerException } # PowerShell이 한 겹 더 감싼다
		if ($e -and $e.SocketErrorCode -eq [Net.Sockets.SocketError]::TimedOut) { return [pscustomobject]@{ State = 'timeout'; Bytes = [byte[]]@() } }
		return [pscustomobject]@{ State = 'closed'; Bytes = [byte[]]@() }
	}
}

# 서버가 끊을 때까지(받은 바이트는 버린다). 끊겼으면 true
function Wait-Closed($t, $ms = 2000)
{
	$watch = [Diagnostics.Stopwatch]::StartNew()
	while ($watch.ElapsedMilliseconds -lt $ms)
	{
		$r = Read-Some $t ([int]($ms - $watch.ElapsedMilliseconds) + 1)
		if ($r.State -eq 'closed') { return $true }
		if ($r.State -eq 'timeout') { return $false }
	}
	return $false
}

# Hello를 보내고 Challenge를 받아 SeqNumber 시작값(uint64)을 돌려준다. 못 받으면 $null
function Get-StartSeq($t, [uint64]$no)
{
	Send-Payload $t (New-HelloBytes $no)
	$r = Read-Some $t 2000
	if ($r.State -ne 'data' -or $r.Bytes.Length -ne 12 -or $r.Bytes[2] -ne 2) { return $null } # [길이 10][type 2][시작값 8]
	return [BitConverter]::ToUInt64($r.Bytes, 4)
}

# 로그인 서버에서 세션 키를 받는다(클라 auth) - Redis session:<no>가 새 키로 덮어써진다
function Get-Key($id)
{
	$c = New-GameClient "k_$id"; Say $c "auth $id" 1200
	$out = Close-GameClient $c
	$line = $out | Where-Object { $_ -match '\[세션 키\] no=(\d+) key=([0-9a-f]{64})' } | Select-Object -First 1
	if (-not $line) { return $null }
	$null = $line -match '\[세션 키\] no=(\d+) key=([0-9a-f]{64})'
	return [pscustomobject]@{ No = [uint64]$Matches[1]; Key = $Matches[2] }
}

# 클라 enterdump - 이 시작값으로 만든 입장 요청(헤더 포함) 바이트
function Get-EnterBytes($no, $key, [uint64]$start, $mode = '')
{
	$c = New-GameClient 'dump'; Say $c "enterdump $no $key $start $mode" 300
	$out = Close-GameClient $c
	$line = $out | Where-Object { $_ -match '\[입장 패킷\] ([0-9a-f]+)' } | Select-Object -First 1
	$null = $line -match '\[입장 패킷\] ([0-9a-f]+)'
	$hex = $Matches[1]
	return [byte[]](0..($hex.Length / 2 - 1) | ForEach-Object { [Convert]::ToByte($hex.Substring($_ * 2, 2), 16) })
}

function Test-KeyExists([uint64]$no) { "$(Invoke-Redis @("EXISTS session:$no"))".Trim() -eq '1' }

# 통계 증가분 - 통계 줄은 1초마다라 조금 기다린 뒤 읽는다
$script:Base = @{}
function Save-Stats { Start-Sleep -Milliseconds 1200; foreach ($k in '입장확인실패', '입장거절', '잘못된패킷', '입장기한끊김') { $script:Base[$k] = Get-ServerStat $k } }
function Get-Delta($k) { Start-Sleep -Milliseconds 1200; (Get-ServerStat $k) - $script:Base[$k] }
function Get-Deltas { Start-Sleep -Milliseconds 1200; $d = @{}; foreach ($k in '입장확인실패', '입장거절', '잘못된패킷', '입장기한끊김') { $d[$k] = (Get-ServerStat $k) - $script:Base[$k] }; return $d }
function Format-Delta($d) { "입장확인실패 +$($d['입장확인실패']), 입장거절 +$($d['입장거절']), 잘못된패킷 +$($d['잘못된패킷']), 입장기한끊김 +$($d['입장기한끊김'])" }

# 클라로 정상 입장(받은 키로)
function Test-EnterOk($no, $key)
{
	$c = New-GameClient 'ok'; Say $c "enter $no $key" 800
	return (Close-GameClient $c) -match '\[로그인\] 성공'
}

$results = [ordered]@{}
$cfgOrig = Set-ServerCfg 'entryLimitSec' 5
try
{
	Start-GameServer 'entry_bad'

	# ================================================================ 3-1 Hello·Challenge
	Save-Stats
	$h1 = New-Raw                                  # H-01 아무것도 안 보냄
	$h2 = New-Raw; $h2Start = Get-StartSeq $h2 999001 # H-02 Hello만
	$h1Closed = Wait-Closed $h1 8000; $h2Closed = Wait-Closed $h2 2000
	$d = Get-Deltas
	$results['H-01·H-02'] = @(($h1Closed -and $h2Closed -and $null -ne $h2Start -and $d['입장기한끊김'] -eq 2), "빈 연결 끊김 $h1Closed, Hello만 끊김 $h2Closed(Challenge 받음 $($null -ne $h2Start)), $(Format-Delta $d)")
	$h1.Close(); $h2.Close()

	Save-Stats
	$a = New-Raw; Send-Payload $a ([byte[]](@(1, 0) + (1..7)))       # 9바이트
	$b = New-Raw; Send-Payload $b ([byte[]](@(1, 0) + (1..9)))       # 11바이트
	$ra = Read-Some $a 1500; $rb = Read-Some $b 1500
	$d = Get-Deltas
	$results['H-03'] = @(($ra.State -eq 'closed' -and $rb.State -eq 'closed' -and $d['잘못된패킷'] -eq 2), "9바이트 $($ra.State), 11바이트 $($rb.State)(Challenge 없이), $(Format-Delta $d)")
	$a.Close(); $b.Close()

	Save-Stats
	$a = New-Raw; Send-Payload $a ([byte[]](@(3, 0) + (1..32)))      # Hello 없이 입장 요청 모양(34바이트)
	$c4 = Wait-Closed $a; $d = Get-Deltas
	$results['H-04'] = @(($c4 -and $d['잘못된패킷'] -eq 1), "끊김 $c4, $(Format-Delta $d)")
	$a.Close()

	Save-Stats
	$a = New-Raw; $s = Get-StartSeq $a 999002; Send-Payload $a (New-HelloBytes 999002) # Hello 두 번
	$c5 = Wait-Closed $a; $d = Get-Deltas
	$results['H-05'] = @(($null -ne $s -and $c5 -and $d['잘못된패킷'] -eq 1), "끊김 $c5, $(Format-Delta $d)")
	$a.Close()

	Save-Stats
	$a = New-Raw; Send-Payload $a ([byte[]](@(5, 0) + [BitConverter]::GetBytes([uint64]1))) # 10바이트지만 type = ChatReq
	$c6 = Wait-Closed $a; $d = Get-Deltas
	$results['H-06'] = @(($c6 -and $d['잘못된패킷'] -eq 1), "끊김 $c6, $(Format-Delta $d)")
	$a.Close()

	# ================================================================ 3-2 입장 요청
	$k1 = Get-Key $users[0]
	Save-Stats
	$e1 = New-Raw; $s = Get-StartSeq $e1 $k1.No; Send-Bytes $e1 (Get-EnterBytes $k1.No $k1.Key $s 'seqbad'); $e1c = Wait-Closed $e1
	$e2 = New-Raw; $s = Get-StartSeq $e2 $k1.No; Send-Bytes $e2 (Get-EnterBytes $k1.No $k1.Key $s 'bad'); $e2c = Wait-Closed $e2
	$e5 = New-Raw; $s = Get-StartSeq $e5 $k1.No; Send-Bytes $e5 (Get-EnterBytes $k1.No $k1.Key $s 'bodybad'); $e5c = Wait-Closed $e5
	$d = Get-Deltas
	$keyLeft = Test-KeyExists $k1.No
	$okAfter = Test-EnterOk $k1.No $k1.Key
	$results['E-01·E-02·E-05'] = @(($e1c -and $e2c -and $e5c -and $d['입장확인실패'] -eq 3 -and $keyLeft -and $okAfter),
		"SeqNumber 다름 $e1c, 태그 변조 $e2c, 본문 AccountNo 다름 $e5c, 키 남음 $keyLeft, 이어서 정상 입장 $okAfter, $(Format-Delta $d)")
	$e1.Close(); $e2.Close(); $e5.Close()

	Save-Stats
	$a = New-Raw; $s = Get-StartSeq $a 999003
	$bytes = Get-EnterBytes 999003 ('00' * 32) $s; $bytes[2] = 5            # 평문 type을 ChatReq로
	Send-Bytes $a $bytes; $c3 = Wait-Closed $a; $d = Get-Deltas
	$results['E-03'] = @(($c3 -and $d['잘못된패킷'] -eq 1 -and $d['입장확인실패'] -eq 0), "끊김 $c3, $(Format-Delta $d)")
	$a.Close()

	$k2 = Get-Key $users[1]; $k1 = Get-Key $users[0]
	Save-Stats
	$a = New-Raw; $s = Get-StartSeq $a $k2.No; Send-Bytes $a (Get-EnterBytes $k2.No $k1.Key $s); $c4 = Wait-Closed $a  # 남의 번호 + 내 키
	$d = Get-Deltas
	$results['E-04'] = @(($c4 -and $d['입장확인실패'] -eq 1 -and (Test-KeyExists $k2.No)), "끊김 $c4, 그 번호의 키 남음 $(Test-KeyExists $k2.No), $(Format-Delta $d)")
	$a.Close()

	Save-Stats
	$a = New-Raw; $s = Get-StartSeq $a 987654321; Send-Bytes $a (Get-EnterBytes 987654321 ('11' * 32) $s); $c6 = Wait-Closed $a # 키 없는 번호
	$b = New-Raw; $s = Get-StartSeq $b 999004; $bytes = Get-EnterBytes 999004 ('00' * 32) $s
	$short = [byte[]]$bytes[0..($bytes.Length - 2)]; $short[0] = [byte]($short.Length - 2)       # 길이 33(헤더 길이도 맞춤)
	Send-Bytes $b $short; $c7 = Wait-Closed $b
	$d = Get-Deltas
	$results['E-06·E-07'] = @(($c6 -and $c7 -and $d['입장확인실패'] -eq 1 -and $d['잘못된패킷'] -eq 1), "키 없음 $c6, 길이 33 $c7, $(Format-Delta $d)")
	$a.Close(); $b.Close()

	# ================================================================ 3-3 리플레이
	$k = Get-Key $users[2]
	Save-Stats
	$real = New-Raw; $sReal = Get-StartSeq $real $k.No
	$p = Get-EnterBytes $k.No $k.Key $sReal                                        # 진짜 연결의 입장 요청
	$atk = New-Raw; $null = Get-StartSeq $atk $k.No; Send-Bytes $atk $p; $r2 = Wait-Closed $atk  # R-02 진짜보다 먼저, 다른 연결
	$atk3 = New-Raw; $s3 = Get-StartSeq $atk3 $k.No
	$p3 = [byte[]]$p.Clone(); [Array]::Copy([BitConverter]::GetBytes([uint64]$s3), 0, $p3, 4, 8)   # R-03 SeqNumber만 자기 연결 값으로
	Send-Bytes $atk3 $p3; $r3 = Wait-Closed $atk3
	$keyBefore = Test-KeyExists $k.No
	Send-Bytes $real $p; $realRes = Read-Some $real 2000                         # 진짜 연결 입장 - 입장 응답(통일 모양 27 + 헤더 2)
	$realOk = ($realRes.State -eq 'data' -and $realRes.Bytes.Length -eq 29 -and $realRes.Bytes[2] -eq 4)
	$atk1 = New-Raw; $null = Get-StartSeq $atk1 $k.No; Send-Bytes $atk1 $p; $r1 = Wait-Closed $atk1 # R-01 진짜가 들어간 뒤 다시
	$d = Get-Deltas
	$results['R-01·R-02·R-03'] = @(($r2 -and $r3 -and $r1 -and $keyBefore -and $realOk -and $d['입장확인실패'] -eq 3),
		"먼저 보냄 끊김 $r2, SeqNumber 바꿈 끊김 $r3, 그동안 키 남음 $keyBefore, 진짜 입장 $realOk, 뒤에 다시 보냄 끊김 $r1, $(Format-Delta $d)")
	$real.Close(); $atk.Close(); $atk3.Close(); $atk1.Close()

	Save-Stats
	$c = New-GameClient 'r4'; Say $c "login $($users[3])" 900; Say $c 'loginagain' 600
	$out = Close-GameClient $c; $d = Get-Deltas
	$results['R-04'] = @((($out -match '\[로그인\] 성공') -and ($out -match '\[연결 끊김\]') -and $d['잘못된패킷'] -eq 1), "$(Format-Delta $d)")

	# ================================================================ 3-4 입장 상태
	$k = Get-Key $users[3]
	Save-Stats
	$a = New-Raw; $s = Get-StartSeq $a $k.No; $p = Get-EnterBytes $k.No $k.Key $s
	Send-Bytes $a ([byte[]]($p + $p)); $s1 = Wait-Closed $a 3000                    # S-01 같은 입장 요청을 바로 두 번
	$d = Get-Deltas
	$c = New-GameClient 's1'; Say $c "login $($users[3])" 900; $s1Again = (Close-GameClient $c) -match '\[로그인\] 성공'
	$results['S-01'] = @(($s1 -and $d['잘못된패킷'] -eq 1 -and $s1Again), "끊김 $s1, 새 연결 입장 $s1Again, $(Format-Delta $d)")
	$a.Close()

	$k = Get-Key $users[4]
	$a = New-Raw; $s = Get-StartSeq $a $k.No; Send-Bytes $a (Get-EnterBytes $k.No $k.Key $s); $a.Close() # S-02 보내자마자 끊음
	Start-Sleep -Milliseconds 1500
	$c = New-GameClient 's2'; Say $c "login $($users[4])" 900; $s2Again = (Close-GameClient $c) -match '\[로그인\] 성공'
	Start-Sleep -Milliseconds 1300
	$results['S-02'] = @(($s2Again -and (Get-ServerStat '세션') -eq 0), "재입장 $s2Again, 세션 $(Get-ServerStat '세션')")

	Save-Stats
	$old = New-GameClient 's3a'; Say $old "login $($users[0])" 900
	$new = New-GameClient 's3b'; Say $new "login $($users[0])" 900; Say $new 'chat still-here' 400
	$oldOut = Close-GameClient $old; $newOut = Close-GameClient $new; $d = Get-Deltas
	$results['S-03'] = @((($oldOut -match '\[로그인\] 성공') -and ($oldOut -match '\[연결 끊김\]') -and ($newOut -match '\[로그인\] 성공') -and ($newOut -match 'still-here') -and -not ($newOut -match '\[연결 끊김\]') -and $d['입장거절'] -eq 0),
		"기존 연결 끊김 $([bool]($oldOut -match '\[연결 끊김\]')), 새 연결 입장·채팅 $([bool]($newOut -match 'still-here')), $(Format-Delta $d)")

	# ================================================================ 3-5 입장 뒤 메시지
	foreach ($m in @(
		@('M-01', 'seqskip'), @('M-02', 'tamper'), @('M-03', 'typeflip'), @('M-04', 'replaylast'), @('M-06', 'plain')))
	{
		Save-Stats
		$c = New-GameClient $m[0]; Say $c "login $($users[1])" 900
		if ($m[1] -eq 'replaylast') { Say $c 'chat once' 400 }
		Say $c $m[1] 600
		$out = Close-GameClient $c; $d = Get-Deltas
		$results[$m[0]] = @((($out -match '\[로그인\] 성공') -and ($out -match '\[연결 끊김\]') -and $d['잘못된패킷'] -eq 1), "$($m[1]) - $(Format-Delta $d)")
	}
	# M-05 되돌려 보내기 - 다른 사람의 채팅으로 서버 → 클라 SeqNumber가 (시작값 + 1)인 패킷을 받은 뒤 되돌린다. 서버가 클라에게 기다리는 값도 시작값 + 1이라 ①은 지나고 ②(방향)에서 걸린다
	Save-Stats
	$x = New-GameClient 'm5x'; Say $x "login $($users[1])" 900
	$y = New-GameClient 'm5y'; Say $y "login $($users[2])" 900; Say $y 'chat reflect-me' 500
	Say $x 'reflect' 600
	$xOut = Close-GameClient $x; $null = Close-GameClient $y; $d = Get-Deltas
	$results['M-05'] = @((($xOut -match 'reflect-me') -and ($xOut -match '\[연결 끊김\]') -and $d['잘못된패킷'] -eq 1), "$(Format-Delta $d)")

	Start-Sleep -Milliseconds 1300
	$endSessions = Get-ServerStat '세션'
	Stop-GameServer | Out-Null
}
finally
{
	Restore-ServerCfg $cfgOrig
}

"===== 판정"
foreach ($k in $results.Keys) { Test-Check $k $results[$k][0] $results[$k][1] }
Test-Check '끝난 뒤 세션 0, 서버 정상 종료, 새 덤프 없음' (($endSessions -eq 0) -and ((Get-Content "$GameOut\server.txt" -Encoding Default) -match '종료 완료') -and -not (Get-ChildItem $GameBin -Filter *.dmp | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-15) })) "세션 $endSessions"
Write-Verdict 'entry_bad'
