param([string[]]$Cases = @('1:1'), [int]$RunSec = 30, [string]$OutRoot)
$ErrorActionPreference = 'Stop'
$repo = 'D:\_Arkchimy'
$bin = "$repo\RsaEcho\x64\Release"
$srvDir = "$repo\RsaEcho\Server"
$cliDir = "$repo\RsaEcho\Client"
$cfgPath = "$cliDir\client.cfg"
$orig = [IO.File]::ReadAllBytes($cfgPath)
$utf8 = New-Object Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force $OutRoot | Out-Null
try
{
	foreach ($c in $Cases)
	{
		$p = $c.Split(':'); $s = $p[0]; $t = $p[1]
		$dir = "$OutRoot\s${s}_t${t}"
		New-Item -ItemType Directory -Force $dir | Out-Null
		$text = $utf8.GetString($orig) -creplace 'sessionCnt = \d+;', "sessionCnt = $s;" -creplace 'threadCnt = \d+;', "threadCnt = $t;"
		[IO.File]::WriteAllText($cfgPath, $text, $utf8)
		while (Get-Process RsaEchoServer -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }
		$before = Get-ChildItem $srvDir -Filter 'RsaEchoServer_*_Profiler.txt' -ErrorAction SilentlyContinue | ForEach-Object FullName
		$srv = Start-Process "$bin\RsaEchoServer.exe" -WorkingDirectory $srvDir -PassThru -WindowStyle Hidden -RedirectStandardOutput "$dir\server.txt" -RedirectStandardError "$dir\server_err.txt"
		$ok = $false
		for ($i = 0; $i -lt 50; $i++) { Start-Sleep -Milliseconds 200; if ((Get-Content "$dir\server.txt" -Encoding Default -ErrorAction SilentlyContinue) -match '\[main\] 포트') { $ok = $true; break } }
		if (-not $ok) { Write-Host "[경고] $c 서버 시작 확인 실패"; }
		$cli = Start-Process "$bin\RsaEchoClient.exe" -WorkingDirectory $cliDir -PassThru -WindowStyle Hidden -RedirectStandardOutput "$dir\client.txt" -RedirectStandardError "$dir\client_err.txt"
		Start-Sleep -Seconds $RunSec
		Stop-Process -Id $cli.Id -Force
		Start-Sleep -Seconds 7
		Stop-Process -Id $srv.Id -Force
		Start-Sleep -Seconds 1
		$new = Get-ChildItem $srvDir -Filter 'RsaEchoServer_*_Profiler.txt' | Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime | Select-Object -Last 1
		if ($new) { Move-Item $new.FullName "$dir\profiler.txt" -Force } else { Write-Host "[경고] $c 프로파일 파일 없음" }
		Write-Host "$c 끝"
	}
}
finally
{
	[IO.File]::WriteAllBytes($cfgPath, $orig)
}
