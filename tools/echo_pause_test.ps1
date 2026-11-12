param([string]$Name, [string]$ClientCfg, [int]$TotalMin = 240, [int]$IntervalSec = 300, [int]$WaitMaxSec = 60, [string]$Config = 'Release')
# 장시간 시험: IntervalSec마다 클라에 s(일시정지) → 미수신이 0이 될 때까지 최대 WaitMaxSec 기다림 → s(다시 시작)
# 회차마다 일시정지 직전 미수신, 0이 되기까지 걸린 초, 예상밖끊김 변화를 pause_log.csv에 남긴다. 끝나면 클라·서버에 q
$ErrorActionPreference = 'Stop'
$sp = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Join-Path (Split-Path -Parent $sp) 'Echo' # 저장소의 Echo 폴더
$bin = Join-Path $root "x64\$Config"
$out = Join-Path $env:TEMP "echo_test\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
$enc = New-Object Text.UTF8Encoding($false)
$cliCfgPath = Join-Path $root 'Client\client.cfg'
$cliBackup = [IO.File]::ReadAllBytes($cliCfgPath)
$csv = "$out\pause_log.csv"
"회차,일시정지시각,직전미수신B,0까지초,결과,예상밖끊김전,예상밖끊김후,연결" | Out-File $csv -Encoding utf8

function Start-WithConsole($exe, $dir, $outName)
{
	$line = "/s /c `"`"$exe`" > `"$out\$outName.txt`" 2> `"$out\${outName}_err.txt`"`""
	$cmd = Start-Process cmd.exe -ArgumentList $line -WorkingDirectory $dir -WindowStyle Hidden -PassThru
	for ($i = 0; $i -lt 50; ++$i)
	{
		$child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($cmd.Id)" | Where-Object { $_.Name -like '*.exe' -and $_.Name -ne 'conhost.exe' } | Select-Object -First 1
		if ($child) { return Get-Process -Id $child.ProcessId }
		Start-Sleep -Milliseconds 100
	}
	throw "$exe 시작 실패"
}

function Send-Key($procId, $key)
{
	Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$sp\send_key.ps1", $procId, "$out\send_key.txt", $key -WindowStyle Hidden -Wait
}

# 클라 출력의 마지막 통계 줄 - 일시정지 여부, 미수신 바이트, 예상밖끊김, 연결
function Get-ClientState
{
	$last = Get-Content "$out\client.txt" -Encoding Default -Tail 3 | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
	if ($last -match '\[stat\](.*?) 연결=(\d+) .*미수신=\d+개\((-?\d+)B\).*예상밖끊김=(\d+)')
	{
		return [pscustomobject]@{ Paused = ($Matches[1] -like '*일시정지*'); Connected = [int]$Matches[2]; Pending = [int64]$Matches[3]; Unexpected = [int64]$Matches[4]; Line = $last }
	}
	return $null
}

try
{
	[IO.File]::WriteAllText($cliCfgPath, ($ClientCfg -replace "`r?`n", "`r`n"), $enc)
	$srv = Start-WithConsole (Join-Path $bin 'EchoServer.exe') (Join-Path $root 'Server') 'server'
	Start-Sleep -Seconds 1
	$cli = Start-WithConsole (Join-Path $bin 'EchoClient.exe') (Join-Path $root 'Client') 'client'
	"시작: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') 서버 pid=$($srv.Id) 클라 pid=$($cli.Id)"

	$endTime = (Get-Date).AddMinutes($TotalMin)
	$round = 0
	while ((Get-Date).AddSeconds($IntervalSec) -lt $endTime)
	{
		Start-Sleep -Seconds $IntervalSec
		if ($srv.HasExited -or $cli.HasExited) { "[경고] 프로세스가 끝남 서버=$($srv.HasExited) 클라=$($cli.HasExited)"; break }
		++$round
		$before = Get-ClientState
		$pauseAt = Get-Date
		Send-Key $cli.Id 's'
		$zeroSec = -1
		for ($i = 1; $i -le $WaitMaxSec; ++$i)
		{
			Start-Sleep -Seconds 1
			$st = Get-ClientState
			if ($st -and $st.Paused -and $st.Pending -eq 0) { $zeroSec = $i; break }
		}
		$after = Get-ClientState
		$result = if ($zeroSec -lt 0) { 'FAIL-미수신남음' } elseif ($after.Unexpected -ne $before.Unexpected) { 'FAIL-예상밖끊김' } else { 'OK' }
		"$round,$($pauseAt.ToString('HH:mm:ss')),$($before.Pending),$zeroSec,$result,$($before.Unexpected),$($after.Unexpected),$($after.Connected)" | Out-File $csv -Encoding utf8 -Append
		if ($result -ne 'OK') { "[회차 $round] $result - $($after.Line)" }
		Send-Key $cli.Id 's'
	}

	Send-Key $cli.Id 'q'
	$cliDone = $cli.WaitForExit(30000)
	if (-not $cliDone) { Stop-Process -Id $cli.Id -Force; "[경고] 클라가 끝나지 않아 강제 종료" }
	Start-Sleep -Seconds 2
	Send-Key $srv.Id 'q'
	$srvDone = $srv.WaitForExit(30000)
	if (-not $srvDone) { Stop-Process -Id $srv.Id -Force; "[경고] 서버가 끝나지 않아 강제 종료" }
}
finally
{
	[IO.File]::WriteAllBytes($cliCfgPath, $cliBackup)
}
"끝: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') 클라 끝남=$cliDone 서버 끝남=$srvDone"
"-- 일시정지 회차"
$rows = Import-Csv $csv -Encoding UTF8
"회차=$($rows.Count) OK=$(($rows | Where-Object { $_.'결과' -eq 'OK' }).Count) 0까지초 최대=$(($rows | Measure-Object '0까지초' -Maximum).Maximum)"
$rows | Where-Object { $_.'결과' -ne 'OK' }
"-- client"
Get-Content "$out\client.txt" -Encoding Default | Where-Object { $_ -match '^\[result\]' }
Get-Content "$out\client_err.txt" -Encoding Default
"-- server"
$lines = Get-Content "$out\server.txt" -Encoding Default
"통계 줄 수=$(($lines | Where-Object { $_ -match '^\[\d\d:' }).Count)"
$lines | Where-Object { $_ -match '^\[main\]' }
Get-Content "$out\server_err.txt" -Encoding Default
foreach ($who in 'server', 'client')
{
	$stat = Get-Content "$out\$who.txt" -Encoding Default | Where-Object { $_ -match '메모리=(\d+)MB 핸들=(\d+)' } | ForEach-Object { [pscustomobject]@{ Mem = [int]$Matches[1]; Handle = [int]$Matches[2] } }
	if ($stat)
	{
		$mem = $stat | Measure-Object Mem -Maximum -Minimum
		$hnd = $stat | Measure-Object Handle -Maximum -Minimum
		"-- $who 자원: 메모리 처음=$($stat[0].Mem) 최소=$($mem.Minimum) 최대=$($mem.Maximum) 마지막=$($stat[-1].Mem)MB / 핸들 처음=$($stat[0].Handle) 최소=$($hnd.Minimum) 최대=$($hnd.Maximum) 마지막=$($stat[-1].Handle)"
	}
}
Get-ChildItem (Join-Path $root 'Server') -Filter *.dmp | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-$TotalMin - 5) } | ForEach-Object { "[새 덤프] $($_.Name)" }
