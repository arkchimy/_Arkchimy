param([int]$Rounds = 3, [int]$RunSec = 20, [string]$Tag = "starve")
# README_ERRORS 2-9 기아 재현: 채팅 256세션(하트비트 1초) + 하트비트만 보내는 64세션, chatRoomCnt 16, chatIdleSec 5, chatJoinSec 5, 워커 2
# 조합: mergeSends 0/1 × skip 켬/끔, 회차마다 순서를 돌린다
Set-Location D:\_Arkchimy
$cfgPath = 'networkLibProject\server.cfg'
$orig = [IO.File]::ReadAllBytes($cfgPath)
$enc = New-Object Text.UTF8Encoding($false)
$hbCfg = 'LoadClient\client_chat_hb_tmp.cfg'
$chat = $enc.GetString([IO.File]::ReadAllBytes('LoadClient\client_chat.cfg')).TrimEnd() + "`nheartbeatSec = 1;`n"	# 원본 끝에 줄바꿈이 없어 붙이면 같은 줄이 된다
[IO.File]::WriteAllText($hbCfg, $chat, $enc)
$conds = @(@{m=0;s=1}, @{m=1;s=1}, @{m=0;s=0}, @{m=1;s=0})
$rows = @()
function Med($v) { $a = @($v | Sort-Object); if ($a.Count -eq 0) { return 0 }; $a[[math]::Floor($a.Count / 2)] }
try {
	for ($r = 1; $r -le $Rounds; $r++) {
		$shift = ($r - 1) % $conds.Count
		$order = @($conds[$shift..($conds.Count - 1)]) + @(if ($shift -gt 0) { $conds[0..($shift - 1)] })
		foreach ($c in $order) {
			$t = $enc.GetString($orig) -creplace 'workerCnt = \d+;', 'workerCnt = 2;' -creplace 'chatRoomCnt = \d+;', 'chatRoomCnt = 16;' `
				-creplace 'chatIdleSec = \d+;', 'chatIdleSec = 5;' -creplace 'chatJoinSec = \d+;', 'chatJoinSec = 5;' `
				-creplace 'mergeSends = \d+;', "mergeSends = $($c.m);" -creplace 'skipOnSuccess = \d+;', "skipOnSuccess = $($c.s);"
			[IO.File]::WriteAllText($cfgPath, $t, $enc)
			while (Get-Process networkLibProject -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 200 }
			$name = "${Tag}_m$($c.m)_s$($c.s)_r$r"
			.\tools\echo_test.ps1 -Name $name -RunSec $RunSec -ClientCfgs LoadClient/client_chat_hb_tmp.cfg,LoadClient/client_chatIdleHb.cfg | Out-Null
			$o = "$env:TEMP\arkchimy_echo_test\$name"
			$s = @(Get-Content "$o\server.txt" -Encoding Default)
			$stat = @($s | Where-Object { $_ -match '^\[stat\]' -and $_ -notmatch '세션=0 ' } | Select-Object -Skip 2 | Select-Object -SkipLast 1)
			$chatl = @($s | Where-Object { $_ -match '^\[chat\] 입장' })
			$lastStat = ($s | Where-Object { $_ -match '^\[stat\]' } | Select-Object -Last 1)
			$lastChat = ($chatl | Select-Object -Last 1)
			$c0 = @(Get-Content "$o\client0.txt" -Encoding Default)
			$c1 = @(Get-Content "$o\client1.txt" -Encoding Default)
			$maxUs = (@($c0 | ForEach-Object { if ($_ -match 'max=(\d+)') { [double]$Matches[1] } }) | Measure-Object -Maximum).Maximum
			$row = [pscustomobject]@{
				Round = $r; Merge = $c.m; Skip = $c.s
				RecvPkt = Med ($stat | ForEach-Object { if ($_ -match '수신패킷/s=(\d+)') { [double]$Matches[1] } })
				PktPerSend = Med ($stat | ForEach-Object { if ($_ -match '패킷/송신완료=([\d.]+)') { [double]$Matches[1] } })
				Deliver = Med (@($chatl | Select-Object -Skip 2 | Select-Object -SkipLast 1) | ForEach-Object { if ($_ -match '전달/s=([\d.]+)') { [double]$Matches[1] } })
				MaxLatMs = [math]::Round($maxUs / 1000, 1)
				IdleCut = [regex]::Match($lastChat, '무응답끊김=(\d+)').Groups[1].Value
				Overflow = [regex]::Match($lastStat, '넘침끊김=(\d+)').Groups[1].Value
				ChatClose = [regex]::Match((($c0 | Select-String 'unexpectedClose' | Select-Object -Last 1).Line), 'unexpectedClose=(\d+)').Groups[1].Value
				HbClose = [regex]::Match((($c1 | Select-String 'unexpectedClose' | Select-Object -Last 1).Line), 'unexpectedClose=(\d+)').Groups[1].Value
				StatLines = @($s | Where-Object { $_ -match '^\[stat\]' }).Count
				Ended = [bool]($s | Select-String '종료 완료')
			}
			$rows += $row
			$row | Format-Table -AutoSize | Out-String -Width 300 | Write-Output
		}
	}
}
finally {
	[IO.File]::WriteAllBytes($cfgPath, $orig)
	Remove-Item $hbCfg -ErrorAction SilentlyContinue
	$rows | Export-Csv "$env:TEMP\arkchimy_echo_test\${Tag}_runs.csv" -NoTypeInformation -Encoding UTF8
}
"=== 조합별 중앙값 ==="
$rows | Group-Object Merge, Skip | ForEach-Object {
	$g = $_.Group
	[pscustomobject]@{
		Merge = $g[0].Merge; Skip = $g[0].Skip
		RecvPkt = Med $g.RecvPkt; PktPerSend = Med $g.PktPerSend; Deliver = Med $g.Deliver
		MaxLatMs = Med $g.MaxLatMs; IdleCut = ($g.IdleCut -join '/'); Overflow = ($g.Overflow -join '/')
		ChatClose = ($g.ChatClose -join '/'); HbClose = ($g.HbClose -join '/'); StatLines = ($g.StatLines -join '/')
	}
} | Format-Table -AutoSize | Out-String -Width 300
