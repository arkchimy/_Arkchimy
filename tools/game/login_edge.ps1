# 로그인 서버·게임 서버 입장/암호화 경계 시험 - cpp-reviewer가 설계한 테스트 케이스 TC-L01–L12(기존 login.ps1이 지나가지 않는 경로)
# 1부: 잘못된 입력·길이 경계·세션 키 수명·이미 접속 중·처리 중 끊김·가로챈 입장 패킷·대기열 상한·Redis 재시작·멈춤·MySQL 끊김
# 2부: 요청이 남은 채 종료(L11)   3부: 아무것도 보내지 않는 연결로 세션 칸 고갈(L12, maxSession 20)
# 주의: DB 테이블을 잠시 잠근다(LOCK TABLES) - 시험용 DB에서만 돌린다
param([switch]$SkipSlow) # 27초 대기(L04 b)를 건너뛴다
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss'
$id = "le$tag"
$idB = "lb$tag"
New-TestCredential $id
New-TestCredential $idB

# ---------------------------------------------------------------- 공통 도구
function Open-Raw($port)
{
	$c = New-Object Net.Sockets.TcpClient
	$c.NoDelay = $true
	$c.Connect('127.0.0.1', $port)
	return $c
}

function Send-Raw($c, [byte[]]$b)
{
	try { $c.GetStream().Write($b, 0, $b.Length); return $true } catch { return $false }
}

# 받은 바이트를 모두 모은다 - 상대가 닫거나(closed/reset) 시간이 지날 때(timeout)까지. State와 Bytes
function Read-Until($c, $ms = 1500, $want = 0)
{
	$bytes = New-Object System.Collections.Generic.List[byte]
	$state = 'timeout'
	$buf = New-Object byte[] 8192
	$deadline = [DateTime]::Now.AddMilliseconds($ms)
	while ([DateTime]::Now -lt $deadline)
	{
		$c.ReceiveTimeout = [Math]::Max(50, [int]($deadline - [DateTime]::Now).TotalMilliseconds)
		try { $n = $c.GetStream().Read($buf, 0, $buf.Length) }
		catch
		{
			# PowerShell이 .NET 예외를 한 겹 더 감싼다(MethodInvocationException → IOException → SocketException) - 안쪽 SocketException을 찾는다
			$ex = $_.Exception
			while ($ex -and -not ($ex -is [Net.Sockets.SocketException])) { $ex = $ex.InnerException }
			$state = if ($ex -and $ex.ErrorCode -eq 10060) { 'timeout' } else { 'closed' }
			break
		}
		if ($n -eq 0) { $state = 'closed'; break }
		$bytes.AddRange([byte[]]$buf[0..($n - 1)])
		if ($want -gt 0 -and $bytes.Count -ge $want) { $state = 'data'; break }
	}
	return [pscustomobject]@{ State = $state; Bytes = $bytes.ToArray() }
}

function Rand([int]$n) { $b = New-Object byte[] $n; (New-Object Random).NextBytes($b); return $b }
function Hex([byte[]]$b) { ($b | ForEach-Object { $_.ToString('x2') }) -join '' }
function FromHex([string]$h) { [byte[]]($(for ($i = 0; $i -lt $h.Length; $i += 2) { [Convert]::ToByte($h.Substring($i, 2), 16) })) }

# 통계 줄이 1초마다 나오므로 조금 기다린 뒤 읽는다
function Wait-Stat { Start-Sleep -Milliseconds 1200 }
function Get-StatLines($file) { @(Get-Content "$GameOut\$file" -Encoding Default | Where-Object { $_ -match '\[stat\]' }) }
function Get-StatMax($file, $name, $fromIndex)
{
	$max = -1
	$lines = Get-StatLines $file
	for ($i = $fromIndex; $i -lt $lines.Count; ++$i) { if ($lines[$i] -match "$name=(\d+)") { $max = [Math]::Max($max, [int64]$Matches[1]) } }
	return $max
}

# 별도 mysql 세션이 테이블을 잠가 서버의 읽기를 붙잡는다(서버 읽기 시간 제한 30초보다 짧게)
function Lock-Table($table, $sec)
{
	$kv = Read-DbCfg
	$env:MYSQL_PWD = $kv['password']
	try
	{
		$p = Start-Process $MySqlExe -ArgumentList "-h $($kv['host']) -P $($kv['port']) -u $($kv['user']) $($kv['database']) -e `"LOCK TABLES $table WRITE; SELECT SLEEP($sec); UNLOCK TABLES;`"" -PassThru -WindowStyle Hidden
	}
	finally { Remove-Item Env:MYSQL_PWD }
	Start-Sleep -Milliseconds 700 # 락을 잡을 시간
	return $p
}

function Get-Key($accountID)
{
	$k = New-GameClient 'key'; Say $k "auth $accountID" 600
	$out = Close-GameClient $k
	$line = $out | Where-Object { $_ -match '\[세션 키\] no=(\d+) key=([0-9a-f]{64})' } | Select-Object -First 1
	if ($line -match 'no=(\d+) key=([0-9a-f]{64})') { return [pscustomobject]@{ No = $Matches[1]; Key = $Matches[2] } }
	return $null
}

function Test-LoginOk($out) { [bool]($out -match '\[로그인\] 성공') }
$LOGIN_RES_AUTHFAIL = '0300020001'
$LOGIN_RES_DBERROR = '0300020004'

# ================================================================ 1부
Start-GameServer 'login_edge'

# ---------------------------------------------------------------- TC-L01 로그인 서버: 키 나누기 전 잘못된 입력
"===== TC-L01"
Wait-Stat
$bad0 = (Get-LoginStat '잘못된패킷') + (Get-LoginStat '비정상헤더')
$hdr = [byte[]](0x22, 0, 5, 0)
$l01 = [ordered]@{
	'공개 키 0' = $hdr + (New-Object byte[] 32)
	'공개 키 1(저차 점)' = $hdr + [byte[]](1) + (New-Object byte[] 31)
	'짧음(길이 10)' = [byte[]](0x0A, 0, 5, 0) + (Rand 8)
	'한 바이트 긺(35)' = [byte[]](0x23, 0, 5, 0) + (Rand 33)
	'종류가 틀림' = [byte[]](0x22, 0, 3, 0) + (Rand 32)
	'길이 0' = [byte[]](0, 0)
	'길이 4097' = [byte[]](0x01, 0x10) + (Rand 16)
}
$l01Res = [ordered]@{}
foreach ($k in $l01.Keys)
{
	$c = Open-Raw 33002
	Send-Raw $c $l01[$k] | Out-Null
	$l01Res[$k] = Read-Until $c 1500
	$c.Close()
}
$c = Open-Raw 33002; Send-Raw $c ($hdr + (Rand 32) + $hdr + (Rand 32)) | Out-Null
$twoHello = Read-Until $c 1500; $c.Close()
$c = Open-Raw 33002; Send-Raw $c ($hdr + (Rand 32)) | Out-Null
$okHello = Read-Until $c 1500 100
$okHelloAfter = Read-Until $c 800; $c.Close()
Wait-Stat
$bad1 = (Get-LoginStat '잘못된패킷') + (Get-LoginStat '비정상헤더')

# ---------------------------------------------------------------- TC-L02 게임 서버: 입장 전 잘못된 LoginReq, 반복 실패
"===== TC-L02"
$gBad0 = (Get-ServerStat '잘못된패킷') + (Get-ServerStat '비정상헤더')
$l02 = [ordered]@{ '길이 25' = [byte[]](0x19, 0, 1, 0) + (Rand 23); '길이 27' = [byte[]](0x1B, 0, 1, 0) + (Rand 25); '길이 1' = [byte[]](1, 0, 1) }
$l02Res = [ordered]@{}
$challengeOk = 0
foreach ($k in $l02.Keys)
{
	$c = Open-Raw 33000
	$ch = Read-Until $c 1500 20
	if ($ch.Bytes.Count -eq 20 -and (Hex $ch.Bytes[0..3]) -eq '12002000') { ++$challengeOk }
	Send-Raw $c $l02[$k] | Out-Null
	$l02Res[$k] = Read-Until $c 1500
	$c.Close()
}
$fakeReq = [byte[]](0x1A, 0, 1, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F) + (New-Object byte[] 16)
$c = Open-Raw 33000; $null = Read-Until $c 1500 20
$authFails = 0
for ($i = 0; $i -lt 51; ++$i)
{
	Send-Raw $c $fakeReq | Out-Null
	$r = Read-Until $c 1500 5
	if ((Hex $r.Bytes) -eq $LOGIN_RES_AUTHFAIL) { ++$authFails } else { break }
}
$repeatStillOpen = (Read-Until $c 300).State -eq 'timeout'
$c.Close()
$c = Open-Raw 33000; $null = Read-Until $c 1500 20
Send-Raw $c ($fakeReq + $fakeReq) | Out-Null
$twoReq = Read-Until $c 2000; $c.Close()
Wait-Stat
$gBad1 = (Get-ServerStat '잘못된패킷') + (Get-ServerStat '비정상헤더')
$gDbWait = Get-ServerStat 'DB대기'

# ---------------------------------------------------------------- TC-L03 아이디·비밀번호 길이 경계
"===== TC-L03"
$id15 = ("e$tag" + 'x' * 15).Substring(0, 15)
$id16 = ("f$tag" + 'x' * 16).Substring(0, 16)
$pw64 = 'p' * 64
$u = New-GameClient 'l03'
Say $u "register $id15 $pw64" 700                                   # 15바이트 아이디 + 64바이트 비밀번호
Say $u "register $id16" 500                                         # 16바이트 아이디
Say $u "register g$tag $('q' * 7)" 500                              # 비밀번호 7
Say $u "register h$tag $('q' * 8)" 700                              # 비밀번호 8
Say $u "register i$tag $('q' * 65)" 500                             # 비밀번호 65
$regOut = @((Close-GameClient $u) | Where-Object { $_ -match '^\[가입\]' })
Wait-Stat
$lBad0 = Get-LoginStat '잘못된패킷'; $lFail0 = Get-LoginStat '로그인실패'
$u = New-GameClient 'l03b'
Say $u "login $id15 $pw64" 900
Say $u 'inv' 500
$login15 = Close-GameClient $u
$u = New-GameClient 'l03c'
Say $u "login $id16" 600
Say $u "login $id15 $('p' * 65)" 600
$loginBad = @((Close-GameClient $u) | Where-Object { $_ -match '^\[로그인 서버\]' })
Wait-Stat
$lBad1 = Get-LoginStat '잘못된패킷'; $lFail1 = Get-LoginStat '로그인실패'

# ---------------------------------------------------------------- TC-L04 세션 키 수명
"===== TC-L04"
$k = Get-Key $id
$ttl = "$(Invoke-Redis @("TTL session:$($k.No)"))".Trim()
if (-not $SkipSlow)
{
	Start-Sleep 26
	$e = New-GameClient 'l04b'; Say $e "enter $($k.No) $($k.Key)" 700
	$l04b = Close-GameClient $e
}
$k1 = Get-Key $id
$k2 = Get-Key $id
$e = New-GameClient 'l04c1'; Say $e "enter $($k1.No) $($k1.Key)" 700; $l04c1 = Close-GameClient $e
$exists = "$(Invoke-Redis @("EXISTS session:$($k2.No)"))".Trim()
$e = New-GameClient 'l04c2'; Say $e "enter $($k2.No) $($k2.Key)" 700; $l04c2 = Close-GameClient $e
# d) 두 연결이 같은 키로 거의 동시에
$kd = Get-Key $id
$f1 = New-GameClient 'l04d1'; $f2 = New-GameClient 'l04d2'
Start-Sleep -Milliseconds 600
$f1.StandardInput.WriteLine("enter $($kd.No) $($kd.Key)"); $f2.StandardInput.WriteLine("enter $($kd.No) $($kd.Key)")
$f1.StandardInput.Flush(); $f2.StandardInput.Flush()
Start-Sleep -Milliseconds 1200
Wait-Stat
$dLoginCnt = Get-ServerStat '로그인'
$dOut = @(Close-GameClient $f1) + @(Close-GameClient $f2)
# e) 남의 번호에 내 키
$kB = Get-Key $idB
$kMe = Get-Key $id
$e = New-GameClient 'l04e1'; Say $e "enter $($kB.No) $($kMe.Key)" 700; $l04e1 = Close-GameClient $e
$e = New-GameClient 'l04e2'; Say $e "enter $($kB.No) $($kB.Key)" 700; $l04e2 = Close-GameClient $e
$e = New-GameClient 'l04e3'; Say $e "enter $($kMe.No) $($kMe.Key)" 700; $l04e3 = Close-GameClient $e
$l04Keys = "$(Invoke-Redis @('DBSIZE'))".Trim()

# ---------------------------------------------------------------- TC-L05 이미 접속 중, 끊은 직후 다시 입장
"===== TC-L05"
$a = New-GameClient 'l05a'; Say $a "login $id" 900
$b = New-GameClient 'l05b'; Say $b "login $id" 900
Start-Sleep -Milliseconds 500
Say $b "login $id" 900
Say $b 'inv' 500
Wait-Stat
$l05LoginCnt = Get-ServerStat '로그인'
$aOut = Close-GameClient $a
$bOut = Close-GameClient $b
$l05Already = 0; $l05Ok = 0
for ($i = 0; $i -lt 20; ++$i)
{
	$x = New-GameClient 'x'; Say $x "login $id" 800; $null = Close-GameClient $x
	$y = New-GameClient 'y'; Say $y "login $id" 800; Say $y 'inv' 300
	$yOut = Close-GameClient $y
	if (Test-LoginOk $yOut) { ++$l05Ok; continue }
	if ($yOut -match '이미 접속 중') { ++$l05Already }
	Start-Sleep -Milliseconds 300
	$z = New-GameClient 'z'; Say $z "login $id" 800; Say $z 'inv' 300
	if (Test-LoginOk (Close-GameClient $z)) { ++$l05Ok }
}

# ---------------------------------------------------------------- TC-L06 DB 스레드가 입장을 처리하는 중에 클라가 끊음
"===== TC-L06"
$idDc = "dc$tag"
New-TestCredential $idDc # accounts 행 없음 - 처음 입장 때 LoadOrCreate
Wait-Stat
$acc0 = Get-ServerStat '계정'
$statFrom = (Get-StatLines 'server.txt').Count
$lock = Lock-Table 'accounts' 8
$a = New-GameClient 'l06a'; Say $a "login $idDc" 1000
$a.Kill()
$b6 = New-GameClient 'l06b'; Say $b6 "login $idDc" 300
$lock.WaitForExit(15000) | Out-Null
Start-Sleep 2
$b6Out = Close-GameClient $b6
$c6 = New-GameClient 'l06c'; Say $c6 "login $idDc" 900; Say $c6 'inv' 400
$c6Out = Close-GameClient $c6
Wait-Stat
$l06DbMax = Get-StatMax 'server.txt' 'DB대기' $statFrom
$acc1 = Get-ServerStat '계정'

# ---------------------------------------------------------------- TC-L07 가로챈 LoginReq를 다른 연결에서 다시 보냄
"===== TC-L07"
$k7 = Get-Key $id
$x = New-GameClient 'l07x'; Start-Sleep -Milliseconds 500; Say $x "enterdump $($k7.No) $($k7.Key)" 300
$xOut = Close-GameClient $x
$dump = if ("$xOut" -match '\[입장 패킷\] ([0-9a-f]+)') { $Matches[1] } else { '' }
$r = Open-Raw 33000; $null = Read-Until $r 1500 20
Send-Raw $r (FromHex $dump) | Out-Null
$replay = Read-Until $r 1500 5; $r.Close()
$y = New-GameClient 'l07y'; Say $y "enter $($k7.No) $($k7.Key)" 800
$l07y = Close-GameClient $y

# ---------------------------------------------------------------- TC-L10 대기열 상한(게임 500, 로그인 200)
"===== TC-L10"
$idQ = "dq$tag"
New-TestCredential $idQ
$statFrom = (Get-StatLines 'server.txt').Count
$lock = Lock-Table 'accounts' 20
$q = New-GameClient 'l10q'; Say $q "login $idQ" 1500 # DB 스레드가 LoadOrCreate에서 막힌다
$raws = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt 510; ++$i)
{
	$rc = Open-Raw 33000
	[void]$raws.Add($rc)
}
foreach ($rc in $raws) { $null = Read-Until $rc 1000 20 }
foreach ($rc in $raws) { Send-Raw $rc $fakeReq | Out-Null }
$lockStillHeld = -not $lock.HasExited # 510개를 다 보낼 때까지 락이 잡혀 있어야 대기열이 찬다
$lock.WaitForExit(30000) | Out-Null
Start-Sleep 3
# 응답 내용으로 나눈다 - DbError = 대기열이 차서 바로 거절, AuthFail = 대기열에서 처리됨(없는 번호)
$fastDbError = 0; $lateAuthFail = 0
foreach ($rc in $raws)
{
	$rr = Read-Until $rc 500 5
	switch (Hex $rr.Bytes) { $LOGIN_RES_DBERROR { ++$fastDbError } $LOGIN_RES_AUTHFAIL { ++$lateAuthFail } }
}
foreach ($rc in $raws) { $rc.Close() }
Say $q 'inv' 500
$qOut = Close-GameClient $q
Wait-Stat
$l10DbMax = Get-StatMax 'server.txt' 'DB대기' $statFrom
$l10DbAfter = Get-ServerStat 'DB대기'
# 로그인 200 - 인증 스레드를 credentials 락으로 붙잡고 동시 로그인 210개
$statFrom = (Get-StatLines 'login.txt').Count
$lock = Lock-Table 'credentials' 8
$fl = New-GameClient 'l10f'; Say $fl "authflood 210 $id" 100
$fl.StandardInput.WriteLine('quit'); $fl.StandardInput.Flush()
$lock.WaitForExit(15000) | Out-Null
$fl.WaitForExit(40000) | Out-Null
$flOut = "$($fl.Reader.Result)"
Wait-Stat
$l10AuthMax = Get-StatMax 'login.txt' '인증대기' $statFrom
for ($i = 0; $i -lt 40 -and (Get-LoginStat '인증대기') -ne 0; ++$i) { Start-Sleep 1 } # 남은 해시(최대 200 × 약 63ms)가 끝나기를
$l10AuthDrainSec = $i
$l10AuthAfter = Get-LoginStat '인증대기'

# ---------------------------------------------------------------- TC-L08 Redis 재시작(A), Redis 멈춤(B)
"===== TC-L08"
$ownRedis = $null -ne $RedisProc
$k8 = Get-Key $id
Stop-Redis; Start-Redis
$g = New-GameClient 'l08a'; Say $g "login $id" 1200; Say $g 'inv' 400
$l08a3 = Close-GameClient $g
$g = New-GameClient 'l08a4'; Say $g "enter $($k8.No) $($k8.Key)" 700
$l08a4 = Close-GameClient $g
Stop-Redis
$g = New-GameClient 'l08a5'; Say $g "login $id" 300
$null = Close-GameClient $g
Start-Redis
Start-Sleep 1
$g = New-GameClient 'l08a6'; Say $g "login $id" 600
$l08a5in = Close-GameClient $g
Start-Sleep 6
$g = New-GameClient 'l08a7'; Say $g "login $id" 900
$l08a5out = Close-GameClient $g
# B: Redis 프로세스를 멈춘다(연결은 되지만 응답 없음)
if (-not ('W.Nt' -as [type]))
{
	Add-Type -Namespace W -Name Nt -MemberDefinition '[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h); [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);'
}
$p8 = New-GameClient 'l08p'; Say $p8 "login $id" 900
$statFrom = (Get-StatLines 'server.txt').Count
[void][W.Nt]::NtSuspendProcess($RedisProc.Handle)
$w8 = [Diagnostics.Stopwatch]::StartNew()
$h = New-GameClient 'l08h'; Say $h "login $id" 100; Say $h "login $id" 100; Say $h "login $id" 100
$hOut = Close-GameClient $h
$w8.Stop()
$en = New-GameClient 'l08en'; Say $en "enter 1 $('00' * 32)" 100
$wInv = [Diagnostics.Stopwatch]::StartNew(); Say $p8 'inv' 1500; $wInv.Stop()
$enOut = Close-GameClient $en
Start-Sleep 6
[void][W.Nt]::NtResumeProcess($RedisProc.Handle)
Start-Sleep 6
$g = New-GameClient 'l08b2'; Say $g "login $idB" 900 # $id는 p8이 입장해 있다 - 다른 계정으로
$l08bAfter = Close-GameClient $g
$p8Out = Close-GameClient $p8
$l08DbMax = Get-StatMax 'server.txt' 'DB대기' $statFrom

# ---------------------------------------------------------------- TC-L09 MySQL 연결이 끊긴 뒤 첫 로그인·첫 입장
"===== TC-L09"
$idMk = "mk$tag"
New-TestCredential $idMk
$ids = Invoke-GameSql "SELECT id FROM information_schema.processlist WHERE user = (SELECT SUBSTRING_INDEX(CURRENT_USER(), '@', 1)) AND id <> CONNECTION_ID() AND db = DATABASE()"
foreach ($i in $ids) { Invoke-GameSql "KILL $i" | Out-Null }
$l09 = @()
for ($i = 0; $i -lt 3; ++$i)
{
	$m = New-GameClient "l09_$i"; Say $m "login $idMk" 1200; Say $m 'inv' 300
	$mo = Close-GameClient $m
	$l09 += , $mo
	if (Test-LoginOk $mo) { break }
	Start-Sleep -Milliseconds 500
}
$l09Keys = "$(Invoke-Redis @('DBSIZE'))".Trim()

Wait-Stat
$part1GameSessions = Get-ServerStat '세션'
$part1LoginSessions = Get-LoginStat '세션'
$part1DbFail = Get-ServerStat 'DB실패'
Stop-GameServer
$part1Err = @(Get-Content "$GameOut\server_err.txt", "$GameOut\login_err.txt" -Encoding Default)

# ================================================================ 2부: TC-L11 요청이 남은 채로 종료
Start-GameServer 'login_edge_shutdown'
"===== TC-L11"
$idSd = "sd$tag"
New-TestCredential $idSd
$ent = New-GameClient 'l11e'; Say $ent "login $id" 900
$lock = Lock-Table 'accounts' 6
$sd = New-GameClient 'l11sd'; Say $sd "login $idSd" 800
$sdRaw = @(1..5 | ForEach-Object { $rc = Open-Raw 33000; $null = Read-Until $rc 1000 20; Send-Raw $rc $fakeReq | Out-Null; $rc })
Send-QuitKey $GameServerCmd 'GameServer.exe' 'game' | Out-Null
Say $ent 'inv' 300
$regs = @(1..3 | ForEach-Object { New-GameClient "l11r$_" })
$i = 0; foreach ($rg in $regs) { ++$i; $rg.StandardInput.WriteLine("register sr$tag$i"); $rg.StandardInput.Flush() }
Start-Sleep -Milliseconds 100
Send-QuitKey $LoginServerCmd 'LoginServer.exe' 'login' | Out-Null
$gameDone = $false; $loginDone = $false
for ($t = 0; $t -lt 30 -and -not ($gameDone -and $loginDone); ++$t)
{
	Start-Sleep 1
	$gameDone = [bool](Get-Content "$GameOut\server.txt" -Encoding Default | Where-Object { $_ -match '^\[main\] 종료 완료' })
	$loginDone = [bool](Get-Content "$GameOut\login.txt" -Encoding Default | Where-Object { $_ -match '^\[main\] 종료 완료' })
}
foreach ($rc in $sdRaw) { $rc.Close() }
$lock.WaitForExit(10000) | Out-Null
$entOut = Close-GameClient $ent
$null = Close-GameClient $sd
$regOk = 0; foreach ($rg in $regs) { if ((Close-GameClient $rg) -match '\[가입\] 가입했습니다') { ++$regOk } }
Stop-Redis
$sdAccount = "$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE account_id = '$idSd'")".Trim()
$srCnt = "$(Invoke-GameSql "SELECT COUNT(*) FROM credentials WHERE CAST(account_id AS CHAR) LIKE 'sr$tag%'")".Trim()
$part2Err = @(Get-Content "$GameOut\server_err.txt", "$GameOut\login_err.txt" -Encoding Default)
$part2DbFail = if ((Get-Content "$GameOut\server.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1) -match 'DB실패=(\d+)') { [int]$Matches[1] } else { -1 }

# ================================================================ 3부: TC-L12 아무것도 보내지 않는 연결로 세션 칸 고갈
"===== TC-L12"
$orig = Set-ServerCfg 'maxSession' 20
$orig2 = Set-ServerCfg 'slotCnt' 10
$orig3 = Set-ServerCfg 'loginIdleSec' 3  # 기한을 짧게 - 기본값(10초·60초)으로는 시험이 길어진다
$orig4 = Set-ServerCfg 'entryLimitSec' 3
try
{
	Start-GameServer 'login_edge_idle'
	$idle = New-Object System.Collections.ArrayList
	for ($i = 0; $i -lt 20; ++$i)
	{
		$rc = Open-Raw 33002
		if ($i % 4 -eq 0) { Send-Raw $rc ([byte[]](0x22, 0) + (Rand 10)) | Out-Null } # 길이만 보낸 반쪽 Hello
		[void]$idle.Add($rc)
	}
	Start-Sleep -Milliseconds 500
	$u = New-GameClient 'l12a'; Say $u "register x$tag" 1500
	$l12First = @((Close-GameClient $u) | Where-Object { $_ -match '^\[가입\]' })
	Start-Sleep 5 # 기한 3초 + 감시 간격 1초
	$idleOpen = @($idle | Where-Object { (Read-Until $_ 50).State -eq 'timeout' }).Count
	$u = New-GameClient 'l12b'; Say $u "register x$tag" 1500
	$l12Later = @((Close-GameClient $u) | Where-Object { $_ -match '^\[가입\]' })
	# 게임 서버: 빈 연결 20개(EnterChallenge만 받고 가만히) → 바로 새 클라는 칸이 없음 → 기한 뒤 새 클라는 입장
	$gidle = New-Object System.Collections.ArrayList
	for ($i = 0; $i -lt 20; ++$i) { $rc = Open-Raw 33000; $null = Read-Until $rc 500 20; [void]$gidle.Add($rc) }
	$gc = New-GameClient 'l12g1'; Start-Sleep -Milliseconds 800
	$l12GameFull = Close-GameClient $gc
	Start-Sleep 5
	$gIdleOpen = @($gidle | Where-Object { (Read-Until $_ 50).State -eq 'timeout' }).Count
	$gc = New-GameClient 'l12g2'; Say $gc "login x$tag" 1200; Say $gc 'inv' 400
	$l12Game = Close-GameClient $gc
	# 입장한 클라는 기한이 지나도 끊기지 않는다
	$stay = New-GameClient 'l12s'; Say $stay "login $id" 1200
	Start-Sleep 5
	Say $stay 'inv' 500
	$l12Stay = Close-GameClient $stay
	foreach ($rc in @($idle) + @($gidle)) { $rc.Close() }
	Wait-Stat
	$l12Slot = Get-ServerStat '슬롯부족'
	$l12EntryCut = Get-ServerStat '입장기한끊김'
	$l12IdleCut = Get-LoginStat '기한끊김'
	Stop-GameServer | Out-Null
}
finally
{
	Restore-ServerCfg $orig4 # 마지막에 바꾼 것부터 - 뒤의 원본에는 앞의 변경이 들어 있다
	Restore-ServerCfg $orig3
	Restore-ServerCfg $orig2
	Restore-ServerCfg $orig
}
# ================================================================ 판정
"===== 판정"
$closedAll = @($l01Res.Values | Where-Object { $_.State -eq 'closed' -and -not ($_.Bytes.Count -ge 4 -and (Hex $_.Bytes[0..3]) -eq '62000600') }).Count
Test-Check 'L01 잘못된 첫 패킷 7가지 → ServerHello 없이 끊김' ($closedAll -eq 7) "$closedAll/7 ($(($l01Res.Keys | ForEach-Object { "$_=$($l01Res[$_].State)" }) -join ', '))"
Test-Check 'L01 잘못된패킷+비정상헤더 +8(7가지 + Hello 두 개)' (($bad1 - $bad0) -eq 8) "+$($bad1 - $bad0)"
Test-Check 'L01 Hello 두 개를 한 번에 → 끊김' ($twoHello.State -eq 'closed')
Test-Check 'L01 정상 Hello → ServerHello 100바이트, 연결 유지' (($okHello.Bytes.Count -eq 100) -and ((Hex $okHello.Bytes[0..3]) -eq '62000600') -and ($okHelloAfter.State -eq 'timeout')) "$($okHello.Bytes.Count)바이트, 뒤 $($okHelloAfter.State)"
Test-Check 'L02 접속하면 EnterChallenge 20바이트' ($challengeOk -eq 3)
Test-Check 'L02 길이가 틀린 LoginReq 3가지 → 끊김' (@($l02Res.Values | Where-Object { $_.State -eq 'closed' }).Count -eq 3)
Test-Check 'L02 아무 값 LoginReq 51번 → 모두 AuthFail, 연결 유지(지금 동작 기록)' (($authFails -eq 51) -and $repeatStillOpen) "AuthFail ${authFails}번, 유지 $repeatStillOpen"
Test-Check 'L02 LoginReq 두 개를 한 번에 → 끊김' ($twoReq.State -eq 'closed') "$(Hex $twoReq.Bytes) $($twoReq.State)"
Test-Check 'L02 잘못된패킷 +4, DB대기 0으로 돌아옴' ((($gBad1 - $gBad0) -eq 4) -and ($gDbWait -eq 0)) "+$($gBad1 - $gBad0), DB대기 $gDbWait"
Test-Check 'L03 가입 경계 5가지' (($regOut.Count -eq 5) -and ($regOut[0] -match '가입했습니다') -and ($regOut[1] -match '잘못된 아이디') -and ($regOut[2] -match '8~64') -and ($regOut[3] -match '가입했습니다') -and ($regOut[4] -match '8~64')) ($regOut -join ' / ')
Test-Check 'L03 15바이트 아이디 + 64바이트 비밀번호로 로그인·입장' ((Test-LoginOk $login15) -and ($login15 -match '골드'))
Test-Check 'L03 16바이트 아이디·65바이트 비밀번호 로그인 → 로그인 서버가 끊음' ((@($loginBad | Where-Object { $_ -match '끊겼습니다' }).Count -eq 2) -and (($lBad1 - $lBad0) -eq 2) -and ($lFail1 -eq $lFail0)) "$($loginBad -join ' / '), 잘못된패킷 +$($lBad1 - $lBad0), 로그인실패 +$($lFail1 - $lFail0)"
Test-Check 'L04 SET 직후 TTL 28–30' ([int]$ttl -ge 28 -and [int]$ttl -le 30) "TTL $ttl"
if (-not $SkipSlow) { Test-Check 'L04 27초 된 키로 입장 성공' (Test-LoginOk $l04b) }
Test-Check 'L04 덮어쓴 옛 키 → 실패, 새 키는 남고 입장 성공' (($l04c1 -match '입장 확인 실패') -and ($exists -eq '1') -and (Test-LoginOk $l04c2)) "EXISTS $exists"
Test-Check 'L04 두 연결이 같은 키로 동시에 → 성공 1, 실패 1' ((@($dOut -match '\[로그인\] 성공').Count -eq 1) -and (@($dOut -match '입장 확인 실패').Count -eq 1) -and ($dLoginCnt -eq 1)) "성공 $(@($dOut -match '\[로그인\] 성공').Count), 실패 $(@($dOut -match '입장 확인 실패').Count), 로그인 $dLoginCnt"
Test-Check 'L04 남의 번호에 내 키 → 실패, 그 사람 키는 남아 입장 성공' (($l04e1 -match '입장 확인 실패') -and (Test-LoginOk $l04e2) -and (Test-LoginOk $l04e3))
Test-Check 'L04 끝에 Redis 키 0' ($l04Keys -eq '0') "DBSIZE $l04Keys"
Test-Check 'L05 이미 접속 중 → AlreadyLoggedIn, 이전 접속 끊김, 다시 로그인 성공' (($bOut -match '이미 접속 중') -and ($aOut -match '\[연결 끊김\]') -and (Test-LoginOk $bOut) -and ($bOut -match '골드') -and ($l05LoginCnt -eq 1)) "로그인 $l05LoginCnt / A: $(@($aOut | Where-Object { $_ -match '^\[' }) -join ' + ') / B: $(@($bOut | Where-Object { $_ -match '^\[(로그인|연결)' }) -join ' + ')"
Test-Check 'L05 끊은 직후 다시 입장 20번 → 결국 모두 성공' ($l05Ok -eq 20) "성공 $l05Ok/20, 첫 시도 AlreadyLoggedIn ${l05Already}번"
Test-Check 'L06 처리 중 끊긴 입장 → 계정은 만들어지고, 다음 로그인 성공' (($l06DbMax -ge 1) -and ($acc1 -eq $acc0 + 1) -and (Test-LoginOk $c6Out) -and ($c6Out -match '골드')) "DB대기 최대 $l06DbMax, 계정 $acc0→$acc1, 변형 B: $(if (Test-LoginOk $b6Out) { '성공' } elseif ($b6Out -match '이미 접속 중') { '이미 접속 중' } else { '기타' })"
Test-Check 'L07 가로챈 입장 패킷을 다른 연결에서 → AuthFail' ((Hex $replay.Bytes) -eq $LOGIN_RES_AUTHFAIL) "응답 $(Hex $replay.Bytes) / 패킷 $($dump.Length / 2)바이트"
Test-Check 'L07 그 뒤 진짜 키로 입장 성공(가로챈 시도가 키를 지우지 않음)' (Test-LoginOk $l07y)
Test-Check 'L10 게임 DB 대기열 상한 → 넘은 몫은 바로 DbError' (($fastDbError -ge 5) -and ($fastDbError -le 15)) "바로 DbError $fastDbError, 락 뒤 AuthFail $lateAuthFail, DB대기 최대 $l10DbMax, 보내는 동안 락 유지 $lockStillHeld"
Test-Check 'L10 락이 풀린 뒤 나머지는 AuthFail, 대기열 0, 막혀 있던 첫 입장 성공' ((($fastDbError + $lateAuthFail) -eq 510) -and ($l10DbAfter -eq 0) -and (Test-LoginOk $qOut)) "합 $($fastDbError + $lateAuthFail)/510, DB대기 $l10DbAfter"
$flLine = ($flOut -split "`r?`n" | Where-Object { $_ -match '\[동시 로그인\]' } | Select-Object -First 1)
$fastErr = if ($flLine -match '1초안서버오류=(\d+)') { [int]$Matches[1] } else { -1 }
Test-Check 'L10 로그인 인증 대기열 상한 → 넘은 몫은 1초 안에 서버 오류' (($fastErr -ge 5) -and ($fastErr -le 15) -and ($l10AuthMax -ge 190) -and ($l10AuthMax -le 201) -and ($l10AuthAfter -eq 0)) "$flLine / 인증대기 최대 $l10AuthMax, 끝 $l10AuthAfter(락이 풀린 뒤 약 ${l10AuthDrainSec}초에 0)"
Test-Check 'L08A Redis 재시작 직후 첫 로그인 성공(다시 연결)' ((Test-LoginOk $l08a3) -and (($part1Err -match '\[redis\] 다시 연결함').Count -ge 1)) "$(@($l08a3 | Where-Object { $_ -match '^\[로그인' }) -join ' + '), 재시작 뒤 Redis 띄움 $ownRedis"
Test-Check 'L08A 재시작 전 키 → 입장 실패(저장 끈 Redis)' ($l08a4 -match '입장 확인 실패')
Test-Check 'L08A 꺼진 직후 5초 안은 서버 오류, 그 뒤 성공' (($l08a5in -match '서버 오류') -and (Test-LoginOk $l08a5out)) "$(@($l08a5in | Where-Object { $_ -match '^\[로그인' }) -join ' / ')"
if ($ownRedis)
{
	Test-Check 'L08B Redis 멈춤 → 로그인 3번 모두 서버 오류, 5초 안' ((@($hOut -match '서버 오류').Count -eq 3) -and ($w8.Elapsed.TotalSeconds -lt 5)) "$([math]::Round($w8.Elapsed.TotalSeconds, 1))초"
	Test-Check 'L08B Redis 멈춤 중 입장 → DbError, 입장한 클라의 inv는 응답' (($enOut -match 'DB 오류') -and ($p8Out -match '골드')) "inv까지 $([math]::Round($wInv.Elapsed.TotalSeconds, 1))초"
	Test-Check 'L08B 재개 뒤 로그인 성공, DB대기 최대 한 자리' ((Test-LoginOk $l08bAfter) -and ($l08DbMax -lt 10)) "DB대기 최대 $l08DbMax / $(@($l08bAfter | Where-Object { $_ -match '^\[로그인' }) -join ' + ')"
}
Test-Check 'L09 MySQL 연결을 끊은 뒤 첫 로그인 성공(바람직한 동작)' (Test-LoginOk $l09[0]) "시도 $($l09.Count)번: $(($l09 | ForEach-Object { "$(@($_ | Where-Object { $_ -match '^\[로그인' }) -join ' + ')" }) -join ' | '), Redis 키 $l09Keys"
Test-Check '1부 끝: 두 서버 세션 0, DB실패 0, Assert 없음' (($part1GameSessions -eq 0) -and ($part1LoginSessions -eq 0) -and ($part1DbFail -eq 0) -and (-not ($part1Err -match 'Assert'))) "게임 $part1GameSessions, 로그인 $part1LoginSessions, DB실패 $part1DbFail"
Test-Check 'L11 요청이 남은 채 종료 → 두 서버 종료 완료, Assert 없음' ($gameDone -and $loginDone -and (-not ($part2Err -match 'Assert'))) "게임 $gameDone, 로그인 $loginDone"
Test-Check 'L11 막혀 있던 첫 입장의 계정이 만들어짐, DB실패 0' (($sdAccount -eq '1') -and ($part2DbFail -eq 0)) "accounts $sdAccount, DB실패 $part2DbFail"
Test-Check 'L11 종료 중 가입: DB 행 수 = 가입 성공 응답 수' ([int]$srCnt -eq $regOk) "DB $srCnt, 응답 $regOk"
Test-Check 'L12 빈 연결 20개가 칸을 채운 직후 가입 → 실패(칸 없음)' ($l12First -match '끊겼습니다|접속하지 못했습니다') ($l12First -join ' / ')
Test-Check 'L12 로그인 서버: 기한(3초) 뒤 빈 연결이 끊기고 가입 성공' (($idleOpen -eq 0) -and ($l12Later -match '가입했습니다') -and ($l12IdleCut -ge 20)) "열린 빈 연결 $idleOpen/20, 기한끊김 $l12IdleCut, $($l12Later -join ' / ')"
Test-Check 'L12 게임 서버: 빈 연결 20개 직후 새 클라는 칸이 없어 끊김' ($l12GameFull -match '\[연결 끊김\]') "슬롯부족 $l12Slot"
Test-Check 'L12 게임 서버: 기한 뒤 빈 연결이 끊기고 새 클라 입장' (($gIdleOpen -eq 0) -and (Test-LoginOk $l12Game) -and ($l12Game -match '골드') -and ($l12EntryCut -ge 20)) "열린 빈 연결 $gIdleOpen/20, 입장기한끊김 $l12EntryCut"
Test-Check 'L12 입장한 클라는 기한이 지나도 끊기지 않음' ((Test-LoginOk $l12Stay) -and ($l12Stay -match '골드') -and -not ($l12Stay -match '\[연결 끊김\]'))
Write-Verdict 'login_edge'
