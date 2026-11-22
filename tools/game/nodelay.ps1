# TCP_NODELAY 켬/끔 비교 - 같은 봇 부하(입장 → 0.5–3초마다 채팅)에서 채팅 왕복 시간과 게임 서버 CPU를 번갈아 잰다
# 사용: .\tools\game\nodelay.ps1 -Rounds 3 -Sec 120 -Bots 30
# 채팅 왕복 = 봇이 채팅을 보낸 때 → 방송으로 자기 채팅을 받은 때(봇 [lat] 줄, 5초 구간 p50·p99·최대). 첫 구간(입장 중)은 뺀다
# server.cfg의 tcpNoDelay만 바꾸고 끝나면 원래대로 돌린다(로그인 서버도 같은 파일을 읽어 함께 바뀐다)
param(
	[int]$Rounds = 3,
	[int]$Sec = 120,
	[int]$Bots = 30
)
. "$PSScriptRoot\GameTestCommon.ps1"
$botExe = Join-Path $GameBin 'GameBot.exe'

function Get-Median($values)
{
	$s = @($values | Sort-Object)
	if ($s.Count -eq 0) { return -1 }
	return $s[[int][Math]::Floor(($s.Count - 1) / 2)]
}

function Invoke-OneRun($noDelay, $round)
{
	$orig = Set-ServerCfg 'tcpNoDelay' $noDelay
	try
	{
		Start-GameServer "nodelay_${noDelay}_r$round"
		$out = $GameOut
		$game = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($GameServerCmd.Id)" | Where-Object { $_.Name -eq 'GameServer.exe' }
		$gp = Get-Process -Id $game.ProcessId
		$botProc = Start-Process cmd.exe -ArgumentList "/s /c `"`"$botExe`" $Bots $Sec nd > `"$out\bot.txt`" 2> `"$out\bot_err.txt`"`"" -WorkingDirectory $GameBin -WindowStyle Hidden -PassThru
		Start-Sleep 10 # 봇이 입장하는 동안은 CPU에 넣지 않는다
		$cpu0 = $gp.TotalProcessorTime.TotalSeconds
		$t0 = Get-Date
		$botProc.WaitForExit()
		$gp.Refresh()
		$cpuCores = ($gp.TotalProcessorTime.TotalSeconds - $cpu0) / ((Get-Date) - $t0).TotalSeconds
		Start-Sleep 3
		$chat = Get-ServerStat '채팅'
		$overflow = Get-ServerStat '넘침끊김'
		Stop-GameServer | Out-Null
	}
	finally
	{
		Restore-ServerCfg $orig
	}
	$lat = @(Get-Content "$out\bot.txt" -Encoding Default | Where-Object { $_ -match '^\[lat\] t=(\d+) n=(\d+) p50=([\d.]+) p99=([\d.]+) max=([\d.]+)' } | ForEach-Object {
			[pscustomobject]@{ T = [int]$Matches[1]; N = [int]$Matches[2]; P50 = [double]$Matches[3]; P99 = [double]$Matches[4]; Max = [double]$Matches[5] }
		} | Where-Object { $_.T -gt 5 -and $_.N -gt 0 })
	$botLast = Get-Content "$out\bot.txt" -Encoding Default | Where-Object { $_ -match '예상밖끊김' } | Select-Object -Last 1
	$unexpected = if ($botLast -match '예상밖끊김=(\d+)') { [int]$Matches[1] } else { -1 }
	[pscustomobject]@{
		Round = $round; NoDelay = $noDelay; Windows = $lat.Count; Samples = ($lat | Measure-Object N -Sum).Sum
		P50ms = Get-Median ($lat | ForEach-Object { $_.P50 }); P99ms = Get-Median ($lat | ForEach-Object { $_.P99 })
		MaxMs = ($lat | Measure-Object Max -Maximum).Maximum; GameCpu = [Math]::Round($cpuCores, 3)
		Chat = $chat; Overflow = $overflow; BotUnexpected = $unexpected
	}
}

$rows = New-Object System.Collections.ArrayList
for ($r = 1; $r -le $Rounds; ++$r)
{
	$order = if ($r % 2 -eq 1) { 0, 1 } else { 1, 0 } # 회차마다 순서를 바꾼다
	foreach ($v in $order)
	{
		$row = Invoke-OneRun $v $r
		[void]$rows.Add($row)
		$row | Format-Table -AutoSize | Out-String -Width 200
	}
}
$csv = Join-Path $OutRoot 'nodelay_runs.csv'
$rows | Export-Csv $csv -NoTypeInformation -Encoding UTF8
"===== 회차별 (구간 p50·p99는 5초 구간 값의 중앙값, ms) - $csv"
$rows | Format-Table -Property * -AutoSize | Out-String -Width 300
