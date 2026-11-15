# 게임 서버 시험 공통 - 시험 스크립트가 dot-source 한다: . "$PSScriptRoot\GameTestCommon.ps1"
# 서버·클라는 Game\x64\Release에서 띄우고(그 폴더의 server.cfg·db.cfg·Data를 읽는다), 클라에는 콘솔 명령을 표준 입력으로 흘려 넣는다
# 주의: DB를 만지는 시험은 db.cfg의 데이터베이스(기본 trpg)를 직접 고친다 - 운영 데이터가 있는 DB로 돌리지 않는다
$script:GameRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:GameBin = Join-Path $GameRoot 'Game\x64\Release'
$script:GameExe = Join-Path $GameBin 'GameServer.exe'
$script:LoginExe = Join-Path $GameBin 'LoginServer.exe'
$script:GameCli = Join-Path $GameBin 'GameClient.exe'
# Redis(세션 키) - 경로는 환경 변수 REDIS_DIR(없으면 이 PC의 설치 위치)
$script:RedisDir = if ($env:REDIS_DIR) { $env:REDIS_DIR } else { Join-Path $env:USERPROFILE 'OneDrive\바탕 화면\Redis-x64-3.0.504' }
$script:TestPassword = 'test-password' # 클라의 login <아이디>에서 비밀번호를 빼면 쓰는 값과 같다
$script:AdminExe = Join-Path $GameBin 'AdminTool.exe'
$script:MySqlExe = Join-Path $(if ($env:MYSQL_DIR) { $env:MYSQL_DIR } else { 'C:\Program Files\MySQL\MySQL Server 8.0' }) 'bin\mysql.exe'
$script:OutRoot = Join-Path $env:TEMP 'arkchimy_game_test'

# db.cfg의 키 = 값 (비밀번호가 있어 출력하지 않는다)
function Read-DbCfg
{
	$kv = @{}
	Get-Content (Join-Path $GameRoot 'Game\Server\db.cfg') -Encoding UTF8 | ForEach-Object { if ($_ -match '^\s*(\w+)\s*=\s*(.*?)\s*;') { $kv[$Matches[1]] = $Matches[2] } }
	return $kv
}

# Redis를 127.0.0.1·비밀번호·저장 끔으로 띄운다(설계 login_server.docx 3장). 이미 6379를 누가 쓰고 있으면 그대로 쓴다
function Start-Redis
{
	$script:RedisProc = $null
	if (Get-NetTCPConnection -LocalPort 6379 -State Listen -ErrorAction SilentlyContinue) { return }
	$kv = Read-DbCfg
	$conf = Join-Path $OutRoot 'redis_test.conf' # 비밀번호가 들어 있어 띄운 뒤 지운다(명령줄 인자는 다른 프로세스에 보인다)
	New-Item -ItemType Directory -Force $OutRoot | Out-Null
	[IO.File]::WriteAllText($conf, "port $($kv['redisPort'])`nbind 127.0.0.1`nrequirepass $($kv['redisPassword'])`nsave `"`"`nappendonly no`n", (New-Object Text.UTF8Encoding($false)))
	$script:RedisProc = Start-Process (Join-Path $RedisDir 'redis-server.exe') -ArgumentList "`"$conf`"" -WorkingDirectory $OutRoot -WindowStyle Hidden -PassThru
	Start-Sleep 1
	Remove-Item $conf -ErrorAction SilentlyContinue
}

# redis-cli에 명령을 표준 입력으로 넘긴다(AUTH를 첫 줄로 - 비밀번호를 명령줄에 쓰지 않게). 결과 줄을 돌려준다
function Invoke-Redis([string[]]$commands)
{
	$kv = Read-DbCfg
	# 첫 줄은 빈 줄 - PowerShell이 표준 입력 맨 앞에 붙이는 BOM을 흡수한다(그 줄만 모르는 명령으로 실패. New-ConsoleProcess와 같은 방식)
	$lines = (@('', "AUTH $($kv['redisPassword'])") + $commands) -join "`n"
	$out = $lines | & (Join-Path $RedisDir 'redis-cli.exe') -p $kv['redisPort'] 2>&1
	return @($out | ForEach-Object { "$_" } | Where-Object { $_ -ne '' } | Select-Object -Skip 2) # BOM 줄의 오류와 AUTH의 OK 제외
}

function Stop-Redis
{
	if ($RedisProc -and -not $RedisProc.HasExited)
	{
		Invoke-Redis @('SHUTDOWN NOSAVE') | Out-Null
		Start-Sleep -Milliseconds 500
		if (-not $RedisProc.HasExited) { $RedisProc.Kill() }
	}
}

function Start-ConsoleServer($exe, $outDir, $tag)
{
	return Start-Process cmd.exe -ArgumentList "/s /c `"`"$exe`" > `"$outDir\$tag.txt`" 2> `"$outDir\${tag}_err.txt`"`"" -WorkingDirectory $GameBin -WindowStyle Hidden -PassThru
}

# Redis → 로그인 서버 → 게임 서버 순서로 띄운다
function Start-GameServer($name)
{
	$outDir = Join-Path $OutRoot $name
	New-Item -ItemType Directory -Force $outDir | Out-Null
	$script:GameOut = $outDir
	Start-Redis
	$script:LoginServerCmd = Start-ConsoleServer $LoginExe $outDir 'login'
	$script:GameServerCmd = Start-ConsoleServer $GameExe $outDir 'server'
	Start-Sleep 2
}

function Send-QuitKey($cmdProc, $exeName, $tag)
{
	$child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($cmdProc.Id)" | Where-Object { $_.Name -eq $exeName }
	if ($child)
	{
		Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $GameRoot 'tools\send_key.ps1'), $child.ProcessId, "$GameOut\key_$tag.txt", 'q' -WindowStyle Hidden -Wait
		return $true
	}
	return $false
}

# 서버들에 Q를 보내 정상 종료하고 마지막 [main] 두 줄·stderr·새 덤프를 보여 준다
function Stop-GameServer
{
	$gameOk = Send-QuitKey $GameServerCmd 'GameServer.exe' 'game'
	$loginOk = Send-QuitKey $LoginServerCmd 'LoginServer.exe' 'login'
	Start-Sleep 3
	if (-not $gameOk) { "[경고] 게임 서버 프로세스가 없음" }
	if (-not $loginOk) { "[경고] 로그인 서버 프로세스가 없음" }
	Stop-Redis
	"===== server"
	Get-Content "$GameOut\server.txt" -Encoding Default | Where-Object { $_ -match '^\[main\]' } | Select-Object -Last 2
	Get-Content "$GameOut\server_err.txt" -Encoding Default
	"===== login"
	Get-Content "$GameOut\login.txt" -Encoding Default | Where-Object { $_ -match '^\[main\]' } | Select-Object -Last 2
	Get-Content "$GameOut\login_err.txt" -Encoding Default
	Get-ChildItem $GameBin -Filter *.dmp | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-10) } | ForEach-Object { "[새 덤프] $($_.Name) - python tools\dump_info.py 로 확인" }
}

# 로그인 서버 출력의 마지막 통계 줄에서 값 하나(누적값)
function Get-LoginStat($name)
{
	$line = Get-Content "$GameOut\login.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
	if ($line -match "$name=(\d+)") { [int64]$Matches[1] } else { -1 }
}

function New-ConsoleProcess($exe, $tag)
{
	$psi = New-Object Diagnostics.ProcessStartInfo $exe
	$psi.WorkingDirectory = $GameBin
	$psi.UseShellExecute = $false
	$psi.RedirectStandardInput = $true
	$psi.RedirectStandardOutput = $true
	$psi.StandardOutputEncoding = [Text.Encoding]::Default
	$psi.CreateNoWindow = $true
	$p = [Diagnostics.Process]::Start($psi)
	$p | Add-Member -NotePropertyName Tag -NotePropertyValue $tag
	$p | Add-Member -NotePropertyName Reader -NotePropertyValue $p.StandardOutput.ReadToEndAsync() # 출력이 쌓여 파이프가 막히지 않게
	$p.StandardInput.WriteLine('') # 첫 줄 앞의 BOM을 흡수
	return $p
}

function New-GameClient($tag) { New-ConsoleProcess $GameCli $tag }
function New-AdminTool($tag = 'admin') { New-ConsoleProcess $AdminExe $tag }

function Say($p, $line, $ms = 300)
{
	if (-not $p.HasExited)
	{
		$p.StandardInput.WriteLine($line)
		$p.StandardInput.Flush()
	}
	Start-Sleep -Milliseconds $ms
}

# 클라를 끝내고 출력 줄을 돌려준다(명령 안내 줄 제외)
function Close-GameClient($p)
{
	Say $p '' 50
	Say $p 'quit' 100
	if (-not $p.WaitForExit(5000))
	{
		$p.Kill()
		"[경고] $($p.Tag) 강제 종료" | Write-Host
	}
	return @($p.Reader.Result -split "`r?`n" | Where-Object { $_ -and $_ -notmatch '^명령:|^      (장비|던전|경매|log)|^      help|^접속' })
}

# 차례가 아니면 서버가 무시하므로 파티원 모두 행동을 쉬지 않고 보낸다
function Invoke-DungeonSpam($clients, $rounds = 60)
{
	for ($i = 0; $i -lt $rounds; ++$i)
	{
		foreach ($c in $clients)
		{
			foreach ($k in 'search', 'potion', 'attack 0', 'attack 1', 'attack 2', 'attack 3')
			{
				Say $c $k 30
			}
		}
	}
}

# db.cfg 값으로 SQL 실행(비밀번호는 환경 변수로만 넘기고 출력하지 않는다)
function Invoke-GameSql($sql, [switch]$Table)
{
	$kv = Read-DbCfg
	$env:MYSQL_PWD = $kv['password']
	try
	{
		$format = if ($Table) { '--table' } else { '-N' }
		& $MySqlExe -h $kv['host'] -P $kv['port'] -u $kv['user'] $kv['database'] $format -e $sql 2>&1
	}
	finally
	{
		Remove-Item Env:MYSQL_PWD
	}
}

# 시험용 계정을 DB에 만든다(서버가 꺼져 있을 때 - 처음 로그인할 때 DB에서 읽는다). items: @(@(uid, kind), ...)
# 시험 계정의 로그인 정보 - 기본 비밀번호의 저장 형식 문자열(이 PC의 페퍼로)을 로그인 서버에게 받아 credentials에 넣는다
# 테이블은 로그인 서버가 처음 뜰 때 만들지만, 시험은 서버를 띄우기 전에 계정을 만들기도 해서 여기서도 만든다(같은 정의)
function New-TestCredential($id)
{
	if (-not $script:TestPasswordHash)
	{
		Push-Location $GameBin
		try { $script:TestPasswordHash = "$(@(& $LoginExe --hash $TestPassword) | Where-Object { $_ -match '^\$argon2id\$' } | Select-Object -First 1)".Trim() } finally { Pop-Location }
	}
	Invoke-GameSql "CREATE TABLE IF NOT EXISTS credentials (account_no BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, account_id VARBINARY(15) NOT NULL UNIQUE, password_hash VARCHAR(128) NOT NULL, created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP);" | Out-Null
	Invoke-GameSql "DELETE FROM credentials WHERE account_id = '$id'; INSERT INTO credentials (account_id, password_hash) VALUES ('$id', '$TestPasswordHash');" | Out-Null
}

function New-TestAccount($id, $gold, $items)
{
	New-TestCredential $id
	# 같은 이름으로 다시 돌려도 지난 시험이 남긴 아이템·경매·로그가 섞이지 않게 지운다
	Invoke-GameSql "DELETE FROM items WHERE account_id = '$id'; DELETE FROM logs WHERE account_id = '$id'; DELETE FROM auctions WHERE seller_id = '$id'; DELETE FROM accounts WHERE account_id = '$id';" | Out-Null
	# 시작 골드도 로그에 한 줄 - 골드 = 로그 합계 검사가 맞게(서버가 계정을 만들 때와 같다)
	Invoke-GameSql "INSERT INTO accounts (account_id, gold) VALUES ('$id', $gold); INSERT INTO logs (account_id, kind, gold_delta, detail) VALUES ('$id', 'create', $gold, 'test');" | Out-Null
	if ($items.Count -gt 0)
	{
		# 같은 번호가 지난 시험에서 다른 계정으로 옮겨져 있을 수 있다(낙찰 등) - 번호로도 지운 뒤 넣는다
		$uids = ($items | ForEach-Object { $_[0] }) -join ','
		$values = ($items | ForEach-Object { "($($_[0]), '$id', $($_[1]))" }) -join ','
		Invoke-GameSql "DELETE FROM items WHERE item_uid IN ($uids); INSERT INTO items (item_uid, account_id, item_kind) VALUES $values;" | Out-Null
		$cnt = "$(Invoke-GameSql "SELECT COUNT(*) FROM items WHERE account_id = '$id'")".Trim()
		if ($cnt -ne "$($items.Count)") { "[경고] $id 아이템 $cnt/$($items.Count)개만 만들어짐" | Write-Host }
	}
}

# 계정마다 DB 골드 = 로그 골드 증감 합계인지(바뀔 때마다 저장·증감 쓰기의 검증). 맞지 않는 계정 수를 돌려준다
# like: 아이디 접두어(예: 'at_%') - 그 시험이 만든 계정만 본다(손으로 DB에 넣은 다른 계정에 흔들리지 않게)
function Test-GoldMatchesLog($like = '%')
{
	$rows = Invoke-GameSql "SELECT CAST(a.account_id AS CHAR), a.gold, IFNULL((SELECT SUM(gold_delta) FROM logs l WHERE l.account_id = a.account_id), 0) FROM accounts a WHERE CAST(a.account_id AS CHAR) LIKE '$like'"
	$bad = 0
	foreach ($r in $rows)
	{
		$c = "$r" -split "`t"
		if ($c.Count -ge 3 -and [int64]$c[1] -ne [int64]$c[2]) { ++$bad; "  [불일치] $($c[0]) 골드 $($c[1]) / 로그 합계 $($c[2])" | Write-Host }
	}
	return $bad
}

# server.cfg 값을 잠시 바꾼다 - 돌려받은 원본 바이트로 Restore-ServerCfg
function Set-ServerCfg($key, $value)
{
	$cfg = Join-Path $GameBin 'server.cfg'
	$orig = [IO.File]::ReadAllBytes($cfg)
	$text = [Text.Encoding]::UTF8.GetString($orig) -creplace "(?m)^$key\s*=\s*[^;]*;", "$key = $value;"
	[IO.File]::WriteAllText($cfg, $text, (New-Object Text.UTF8Encoding($false))) # cfg는 BOM 없이(README_ERRORS 1-4)
	return , $orig
}

function Restore-ServerCfg($orig)
{
	[IO.File]::WriteAllBytes((Join-Path $GameBin 'server.cfg'), $orig)
}

# 판정 한 줄 - 시험 끝에 Write-Verdict가 전체 PASS/FAIL을 출력한다
$script:Checks = New-Object System.Collections.ArrayList
function Test-Check($name, $ok, $detail = '')
{
	[void]$Checks.Add([pscustomobject]@{ Name = $name; Ok = [bool]$ok })
	"  [$(if ($ok) { 'OK' } else { 'NG' })] $name$(if ($detail) { " - $detail" })"
}

function Write-Verdict($testName)
{
	$bad = @($Checks | Where-Object { -not $_.Ok }).Count
	"[result] $testName $(if ($bad -eq 0) { 'PASS' } else { "FAIL (${bad}개)" }) - 판정 $($Checks.Count)개"
}

# 서버 출력의 마지막 통계 줄에서 값 하나(누적값)
function Get-ServerStat($name)
{
	$line = Get-Content "$GameOut\server.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
	if ($line -match "$name=(\d+)") { [int64]$Matches[1] } else { -1 }
}

# 대기열 저장이 DB에 반영될 시간을 준 뒤 경매 번호를 읽는다(판매자의 열린 경매 중 가장 최근)
function Get-OpenAuctionID($sellerID)
{
	Start-Sleep -Milliseconds 300
	"$(Invoke-GameSql "SELECT IFNULL(MAX(auction_id), 0) FROM auctions WHERE seller_id = '$sellerID' AND state = 'open'")".Trim()
}
