# RsaEcho·AesEcho 시험 - client.cfg의 sessionCnt·threadCnt만 바꿔 조건마다: 서버 → 클라 RunSec초 → 클라 종료 → 7초 뒤(5초마다 저장) 서버 종료 → 프로파일러 파일을 OutRoot\s<세션>_t<스레드>\profiler.txt로 옮긴다
# 끝나면 client.cfg를 원래 바이트로 되돌린다. 먼저 해당 sln을 Release x64로 빌드해 둔다
# 예: .\tools\echo_compare\echo30.ps1 -Kind Aes -Cases '1:1','4:1','16:2','64:4' -RunSec 30 -OutRoot 실험폴더\aes_echo_pcb\30s
param([ValidateSet('Aes', 'Rsa')][string]$Kind = 'Aes', [string[]]$Cases = @('1:1'), [int]$RunSec = 30, [Parameter(Mandatory = $true)][string]$OutRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) # 저장소 루트(tools\echo_compare의 두 단계 위)
$OutRoot = [IO.Path]::GetFullPath((Join-Path (Get-Location) $OutRoot))
$name = "${Kind}Echo"
$bin = "$repo\$name\x64\Release"
$srvDir = "$repo\$name\Server"
$cliDir = "$repo\$name\Client"
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
		while (Get-Process "${name}Server" -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }
		$before = Get-ChildItem $srvDir -Filter "${name}Server_*_Profiler.txt" -ErrorAction SilentlyContinue | ForEach-Object FullName
		$srv = Start-Process "$bin\${name}Server.exe" -WorkingDirectory $srvDir -PassThru -WindowStyle Hidden -RedirectStandardOutput "$dir\server.txt" -RedirectStandardError "$dir\server_err.txt"
		$ok = $false
		for ($i = 0; $i -lt 50; $i++) { Start-Sleep -Milliseconds 200; if ((Get-Content "$dir\server.txt" -Encoding Default -ErrorAction SilentlyContinue) -match '\[main\] 포트') { $ok = $true; break } }
		if (-not $ok) { Write-Host "[경고] $c 서버 시작 확인 실패" }
		$cli = Start-Process "$bin\${name}Client.exe" -WorkingDirectory $cliDir -PassThru -WindowStyle Hidden -RedirectStandardOutput "$dir\client.txt" -RedirectStandardError "$dir\client_err.txt"
		Start-Sleep -Seconds $RunSec
		Stop-Process -Id $cli.Id -Force
		Start-Sleep -Seconds 7
		Stop-Process -Id $srv.Id -Force
		Start-Sleep -Seconds 1
		$new = Get-ChildItem $srvDir -Filter "${name}Server_*_Profiler.txt" | Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime | Select-Object -Last 1
		if ($new) { Move-Item $new.FullName "$dir\profiler.txt" -Force } else { Write-Host "[경고] $c 프로파일 파일 없음" }
		Write-Host "$c 끝"
	}
}
finally
{
	[IO.File]::WriteAllBytes($cfgPath, $orig)
}
