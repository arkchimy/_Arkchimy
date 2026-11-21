# 인증 분산의 효과 측정 - A: 지금 구조(로그인 서버가 인증), B: 게임 서버가 인증까지 하는 구조를 흉내(ARK_EMULATE_AUTH=1)
# 한 회: 채팅 봇 30개 예열 WarmSec → 입장 폭주 봇 Churn개 BurstSec → 회복 AfterSec. 1초마다 두 서버의 CPU 시간·메모리
param(
	[ValidateSet('A', 'B', 'Prep')][string]$Mode,
	[string]$Tag,
	[int]$Steady = 30,
	[int]$Churn = 100,
	[int]$WarmSec = 60,
	[int]$BurstSec = 60,
	[int]$AfterSec = 60
)
. 'D:\_Arkchimy\tools\game\GameTestCommon.ps1'
$botExe = Join-Path $GameBin 'GameBot.exe'
if ($Mode -eq 'B') { $env:ARK_EMULATE_AUTH = '1' } else { Remove-Item Env:ARK_EMULATE_AUTH -ErrorAction SilentlyContinue }
Start-GameServer "auth_$Tag"
$out = $GameOut
Remove-Item "$out\*.csv", "$out\steady*.txt", "$out\churn*.txt" -ErrorAction SilentlyContinue

if ($Mode -eq 'Prep')
{
	# 폭주용 계정 가입 + 게임 계정 행 만들기(측정에 가입 해시·첫 입장 INSERT가 섞이지 않게)
	$p = Start-Process cmd.exe -ArgumentList "/s /c `"`"$botExe`" $Churn 40 churn 127.0.0.1 33000 33002 churn > `"$out\churn.txt`" 2> `"$out\churn_err.txt`"`"" -WorkingDirectory $GameBin -WindowStyle Hidden -PassThru
	$p.WaitForExit()
	Get-Content "$out\churn.txt" -Encoding Default | Select-Object -Last 2
	Stop-GameServer | Out-Null
	return
}

$total = $WarmSec + $BurstSec + $AfterSec
$steadyProc = Start-Process cmd.exe -ArgumentList "/s /c `"`"$botExe`" $Steady $($total + 5) soak > `"$out\steady.txt`" 2> `"$out\steady_err.txt`"`"" -WorkingDirectory $GameBin -WindowStyle Hidden -PassThru
$t0 = Get-Date
$game = Get-Process GameServer
$login = Get-Process LoginServer
$churnProc = $null
$rows = New-Object System.Collections.ArrayList
$prevGame = $game.TotalProcessorTime.TotalSeconds
$prevLogin = $login.TotalProcessorTime.TotalSeconds
for ($s = 1; $s -le $total; $s++)
{
	if ($s -eq $WarmSec + 1)
	{
		$churnProc = Start-Process cmd.exe -ArgumentList "/s /c `"`"$botExe`" $Churn $BurstSec churn 127.0.0.1 33000 33002 churn > `"$out\churn.txt`" 2> `"$out\churn_err.txt`"`"" -WorkingDirectory $GameBin -WindowStyle Hidden -PassThru
	}
	$next = $t0.AddSeconds($s)
	$wait = ($next - (Get-Date)).TotalMilliseconds
	if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
	$game.Refresh(); $login.Refresh()
	$g = $game.TotalProcessorTime.TotalSeconds; $l = $login.TotalProcessorTime.TotalSeconds
	$phase = if ($s -le $WarmSec) { 'warm' } elseif ($s -le $WarmSec + $BurstSec) { 'burst' } else { 'after' }
	[void]$rows.Add([pscustomobject]@{ Sec = $s; Phase = $phase; GameCpu = [math]::Round($g - $prevGame, 3); GamePrivMB = [math]::Round($game.PrivateMemorySize64 / 1MB, 1);
		LoginCpu = [math]::Round($l - $prevLogin, 3); LoginPrivMB = [math]::Round($login.PrivateMemorySize64 / 1MB, 1) })
	$prevGame = $g; $prevLogin = $l
}
$rows | Export-Csv "$out\proc.csv" -NoTypeInformation -Encoding UTF8
$steadyProc.WaitForExit(30000) | Out-Null
Start-Sleep 3
Stop-GameServer | Out-Null

# 요약
function Summ($phase)
{
	$r = $rows | Where-Object Phase -eq $phase
	[pscustomobject]@{ Phase = $phase; GameCpuAvg = [math]::Round(($r | Measure-Object GameCpu -Average).Average, 3); GameMemMax = ($r | Measure-Object GamePrivMB -Maximum).Maximum;
		LoginCpuAvg = [math]::Round(($r | Measure-Object LoginCpu -Average).Average, 3); LoginMemMax = ($r | Measure-Object LoginPrivMB -Maximum).Maximum }
}
"===== $Tag (모드 $Mode)"
'warm', 'burst', 'after' | ForEach-Object { Summ $_ } | Format-Table -AutoSize | Out-String -Width 200
"--- 채팅 왕복(steady 봇, 5초 구간; 폭주는 t=$WarmSec 부터 $($WarmSec + $BurstSec)초)"
Get-Content "$out\steady.txt" -Encoding Default | Where-Object { $_ -match '^\[lat\]' }
Get-Content "$out\steady.txt" -Encoding Default | Select-Object -Last 2
"--- 폭주 봇"
Get-Content "$out\churn.txt" -Encoding Default | Where-Object { $_ -match '^\[lat\]|bot-result' } | Select-Object -Last 14
"--- 서버 마지막 통계"
Get-Content "$out\server.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]|측정용' } | Select-Object -First 1
Get-Content "$out\server.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
