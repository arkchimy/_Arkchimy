param([string]$Name, [string]$ClientCfg, [string]$ServerCfg = "", [int]$RunSec, [string]$Config = 'Release', [string]$Keys = "")
# 서버·클라를 각자 콘솔로 띄우고 RunSec 뒤 Keys(예: 's:6,s:6,s:4' - 키를 넣고 그만큼 기다림)를 클라에 넣은 뒤 q
# 클라가 끝나면 서버에 q. client.cfg·server.cfg는 끝나면 원래대로
$ErrorActionPreference = 'Stop'
$sp = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Join-Path (Split-Path -Parent $sp) 'Echo' # 저장소의 Echo 폴더
$bin = Join-Path $root "x64\$Config"
$out = Join-Path $env:TEMP "echo_test\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
$enc = New-Object Text.UTF8Encoding($false)
$cliCfgPath = Join-Path $root 'Client\client.cfg'
$srvCfgPath = Join-Path $root 'Server\server.cfg'
$cliBackup = [IO.File]::ReadAllBytes($cliCfgPath)
$srvBackup = [IO.File]::ReadAllBytes($srvCfgPath)

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

try
{
	[IO.File]::WriteAllText($cliCfgPath, ($ClientCfg -replace "`r?`n", "`r`n"), $enc)
	if ($ServerCfg -ne "")
	{
		$text = [IO.File]::ReadAllText($srvCfgPath)
		foreach ($line in ($ServerCfg -split "`n"))
		{
			if ($line -match '^\s*(\w+)\s*=\s*(.+?);?\s*$') { $text = $text -creplace "(?m)^$($Matches[1])\s*=.*$", "$($Matches[1]) = $($Matches[2]);" }
		}
		[IO.File]::WriteAllText($srvCfgPath, $text, $enc)
	}
	$srv = Start-WithConsole (Join-Path $bin 'EchoServer.exe') (Join-Path $root 'Server') 'server'
	Start-Sleep -Seconds 1
	$cli = Start-WithConsole (Join-Path $bin 'EchoClient.exe') (Join-Path $root 'Client') 'client'
	Start-Sleep -Seconds $RunSec
	foreach ($act in ($Keys -split ',' | Where-Object { $_ }))
	{
		$parts = $act -split ':'
		Send-Key $cli.Id $parts[0]
		Start-Sleep -Seconds ([int]$parts[1])
	}
	Send-Key $cli.Id 'q'
	$cliDone = $cli.WaitForExit(20000)
	if (-not $cliDone) { Stop-Process -Id $cli.Id -Force; "[경고] 클라가 끝나지 않아 강제 종료" }
	Start-Sleep -Seconds 2
	Send-Key $srv.Id 'q'
	$srvDone = $srv.WaitForExit(20000)
	if (-not $srvDone) { Stop-Process -Id $srv.Id -Force; "[경고] 서버가 끝나지 않아 강제 종료" }
}
finally
{
	[IO.File]::WriteAllBytes($cliCfgPath, $cliBackup)
	[IO.File]::WriteAllBytes($srvCfgPath, $srvBackup)
}
"== $Name ($Config) 클라 끝남=$cliDone 서버 끝남=$srvDone"
"-- client"
Get-Content "$out\client.txt" -Encoding Default | Where-Object { $_ -match '^\[(main|result)\]' }
Get-Content "$out\client.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
Get-Content "$out\client_err.txt" -Encoding Default
"-- server"
$lines = Get-Content "$out\server.txt" -Encoding Default
"통계 줄 수=$(($lines | Where-Object { $_ -match '^\[\d\d:' }).Count)"
$lines | Where-Object { $_ -match '^\[main\]' }
Get-Content "$out\server_err.txt" -Encoding Default
Get-ChildItem (Join-Path $root 'Server') -Filter *.dmp | ForEach-Object { "[덤프] $($_.Name)" }
