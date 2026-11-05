param(
	[string]$WtRoot = (Join-Path $PSScriptRoot 'wt'),	# 커밋별 worktree 폴더 - 각각 /O2로 빌드해 둔다
	[string[]]$Stages = @('now', 'starve', 'cast', 'batch', 'skip82', 'skipex', 'mixed'),
	[int]$Rounds = 3,
	[int]$RunSec = 15,
	[int]$LimitPerStage = 0,	# 0보다 크면 단계마다 앞의 N건만 (동작 확인용)
	[string]$OutRoot = (Join-Path $PSScriptRoot 'remeasure_out'),
	[string]$Tag = 'run'
)
# 포트폴리오 성능 비교 재측정 (서버·클라 모두 /O2)
# 옛 구조와 비교하는 수치는 그 커밋의 서버·클라를 쓴다. 커밋마다 load_sweep 기능이 달라 같은 절차로 돌리려고 이 스크립트를 따로 둔다
# - 저장소가 아니라 각 worktree의 cfg만 바꾸고, 끝나면(중간에 멈춰도) 원래 바이트로 되돌린다
# - 탐침 클라는 쓰지 않는다(지금 코드에 없어 조건을 통일). 서버 핑은 그 커밋의 기본값 그대로
# - 회차마다 순서를 뒤집고 돌려 시간에 따른 변화가 한쪽에 쏠리지 않게 한다
# - 실행 전·도중의 CPU 상위 프로세스를 함께 남긴다(README_ERRORS 1-24)
$enc = New-Object Text.UTF8Encoding($false)
New-Item -ItemType Directory -Force $OutRoot | Out-Null
$csv = Join-Path $OutRoot "${Tag}_runs.csv"
$log = Join-Path $OutRoot "${Tag}_progress.txt"
function Log([string]$s) { $l = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $s; $l; Add-Content -LiteralPath $log -Value $l -Encoding UTF8 }
function Med($v) { $a = @($v | Sort-Object); if ($a.Count -eq 0) { return 0 }; return $a[[math]::Floor($a.Count / 2)] }
function Grab($lines, [string]$pat) { @($lines | ForEach-Object { if ($_ -match $pat) { [double]$Matches[1] } }) }
function Steady($lines) { @($lines | Select-Object -Skip 2 | Select-Object -SkipLast 1) }	# 연결 직후 2초와 종료 직전 1초 제외

$selfNames = '^(_Total|Idle|networkLibProject.*|LoadClient.*|EchoClient.*|System)(#\d+)?$'
function TopOther {
	$p = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process | Where-Object { $_.Name -notmatch $selfNames -and $_.PercentProcessorTime -ge 15 } |
		Sort-Object PercentProcessorTime -Descending | Select-Object -First 4
	(@($p | ForEach-Object { "$($_.Name):$($_.PercentProcessorTime)" }) -join ' ')
}
# 다른 프로세스가 코어 절반 이상을 쓰면 잠잠해질 때까지 기다린다(최대 90초). 이 스크립트의 powershell은 뺀다
function WaitQuiet {
	$sw = [Diagnostics.Stopwatch]::StartNew()
	while ($sw.Elapsed.TotalSeconds -lt 90) {
		$busy = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process | Where-Object { $_.Name -notmatch $selfNames -and $_.Name -notmatch '^(powershell|WmiPrvSE)' -and $_.PercentProcessorTime -ge 50 }
		if (-not $busy) { return [int]$sw.Elapsed.TotalSeconds }
		Start-Sleep -Seconds 5
	}
	return [int]$sw.Elapsed.TotalSeconds
}
function SetKeys([string]$text, $over) {
	foreach ($k in $over.Keys) {
		$pat = "(?m)^[ \t]*$k[ \t]*=[^\r\n]*"
		if ($text -cmatch $pat) { $text = $text -creplace $pat, "$k = $($over[$k]);" }
		else { $text = $text.TrimEnd() + "`r`n$k = $($over[$k]);`r`n" }	# 줄바꿈부터 붙인다(README_ERRORS 1-20)
	}
	return $text
}

# 한 건: Wt 커밋, 서버 설정, 클라 목록(폴더, 기본 cfg, 덮어쓸 키), 표시 이름
function NewCase($stage, $build, $label, $workers, $srv, $clients, $metaMsg, $room, [int]$sec = 0) {
	$s = @{ workerCnt = $workers; profile = 0 }
	foreach ($k in $srv.Keys) { $s[$k] = $srv[$k] }
	[pscustomobject]@{ Stage = $stage; Build = $build; Label = $label; Workers = $workers; Srv = $s; Clients = $clients; MsgSize = $metaMsg; Room = $room; Sec = $sec }
}
function LoadCli($msg, $over, $room) {
	$o = @{ msgSize = $msg; overSend = $over }
	if ($room -gt 0) { $o.roomSize = $room }
	,@(@{ Proj = 'LoadClient'; Cfg = 'client.cfg'; Over = $o })
}

$cases = @()
# now: 지금 구조(HEAD). 합치기 범위 × skip × 에코/방 4/방 16 × 워커 2·4 + 기본값의 워커 수별 처리량
$head = '3fed5a1'
foreach ($w in 2, 4) {
	foreach ($room in 0, 4, 16) {
		foreach ($ms in 1, 0) {
			foreach ($sk in 1, 0) {
				$chat = if ($room -gt 0) { 256 } else { 0 }
				$cases += NewCase 'now' $head ("room{0} ms{1} skip{2}" -f $room, $ms, $sk) $w @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = $sk; mergeSends = $ms; chatRoomCnt = $chat } (LoadCli 32 16 $room) 32 $room
			}
		}
	}
}
foreach ($w in 1, 3, 5, 6) {
	$cases += NewCase 'now' $head 'room16 ms1 skip1' $w @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = 1; mergeSends = 1; chatRoomCnt = 256 } (LoadCli 32 16 16) 32 16
}
foreach ($w in 2, 4) {
	foreach ($sk in 1, 0) {
		# 지금 헤더는 2바이트라 메시지 상한이 4098B다(옛 3바이트 헤더 커밋의 4099B와 페이로드가 같다)
		$cases += NewCase 'nowbig' $head ("echo4098 ms1 skip{0}" -f $sk) $w @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = $sk; mergeSends = 1; chatRoomCnt = 0 } (LoadCli 4098 1 0) 4098 0
	}
}
# starve: 기본값을 바꾼 근거(README_ERRORS 2-9) - 채팅 256세션(하트비트 1초) + 하트비트만 64세션, 방 16, 워커 2, 20초
foreach ($ms in 0, 1) {
	foreach ($sk in 1, 0) {
		$cl = @(@{ Proj = 'LoadClient'; Cfg = 'client_chat.cfg'; Over = @{ heartbeatSec = 1 } }, @{ Proj = 'LoadClient'; Cfg = 'client_chatIdleHb.cfg'; Over = @{} })
		$cases += NewCase 'starve' $head ("ms{0} skip{1}" -f $ms, $sk) 2 @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = $sk; mergeSends = $ms; chatRoomCnt = 16; chatIdleSec = 5; chatJoinSec = 5 } $cl 32 16 20
	}
}
# cast: 받는 사람마다 모아 보내기 수정 전(6754260) / 후(649aa97), 방 1·4·16, skip 켬·끔, 워커 2·4
foreach ($w in 2, 4) {
	foreach ($room in 1, 4, 16) {
		foreach ($sk in 1, 0) {
			foreach ($b in '6754260', '649aa97') {
				$cases += NewCase 'cast' $b ("room{0} skip{1}" -f $room, $sk) $w @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = $sk; chatRoomCnt = 256 } (LoadCli 32 16 $room) 32 $room
			}
		}
	}
}
# batch: 수신 처리 끝에서 모아 보내기 수정 전(2a91e23) / 후(3a2ef87), GQCS, skip 없음 - 4099×1은 대조 조건
foreach ($w in 2, 4) {
	foreach ($sz in @(@(32, 16), @(4099, 1))) {
		foreach ($b in '2a91e23', '3a2ef87') {
			$cases += NewCase 'batch' $b ("echo{0}x{1}" -f $sz[0], $sz[1]) $w @{ gqcsEx = 0; gqcsBatch = 64 } (LoadCli $sz[0] $sz[1] 0) $sz[0] 0
		}
	}
}
# skip82: 모아 보내기 전 구조(09a49f6)에서 skip 끔/켬, GQCS/GQCSEx - 4099×1은 대조 조건
foreach ($w in 2, 4) {
	foreach ($sz in @(@(32, 16), @(4099, 1))) {
		foreach ($sk in 0, 1) {
			$cl = LoadCli $sz[0] $sz[1] 0	# 09a49f6의 LoadClient에는 msgSize가 없어(무시되고 32B로 돈다) 클라만 바로 다음 커밋 4f8ad22를 쓴다
			$cl[0].Build = '4f8ad22'
			$cases += NewCase 'skip82' '09a49f6' ("gqcs echo{0}x{1} skip{2}" -f $sz[0], $sz[1], $sk) $w @{ gqcsEx = 0; gqcsBatch = 64; skipOnSuccess = $sk } $cl $sz[0] 0
		}
	}
	$cl = LoadCli 32 16 0
	$cl[0].Build = '4f8ad22'
	$cases += NewCase 'skip82' '09a49f6' 'ex64 echo32x16 skip0' $w @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = 0 } $cl 32 0
}
# castfill: 다른 프로세스 때문에 두 번 다 무효가 된 조건 하나를 채운다
$cases += NewCase 'castfill' '649aa97' 'room16 skip0' 2 @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = 0; chatRoomCnt = 256 } (LoadCli 32 16 16) 32 16
# skipex: 모아 보내기 뒤(7de336d)에서 skip 다시 켜기와 GQCS/GQCSEx 2×2 - 앞 문서 7·8장
foreach ($w in 1, 2, 4) {
	foreach ($sz in @(@(32, 16), @(4099, 1))) {
		foreach ($sk in 0, 1) {
			$cases += NewCase 'skipex' '7de336d' ("gqcs echo{0}x{1} skip{2}" -f $sz[0], $sz[1], $sk) $w @{ gqcsEx = 0; gqcsBatch = 64; skipOnSuccess = $sk } (LoadCli $sz[0] $sz[1] 0) $sz[0] 0
		}
	}
}
foreach ($w in 1, 4) {
	foreach ($sk in 0, 1) {
		$cases += NewCase 'skipex' '7de336d' ("ex64 echo32x16 skip{0}" -f $sk) $w @{ gqcsEx = 1; gqcsBatch = 64; skipOnSuccess = $sk } (LoadCli 32 16 0) 32 0
	}
}

# 시작할 때의 cfg 원본을 모아 두고 끝나면 되돌린다
$origCfg = @{}
foreach ($b in @($cases | ForEach-Object { $_.Build } | Sort-Object -Unique)) {
	$p = Join-Path $WtRoot "$b\networkLibProject\server.cfg"
	if (-not (Test-Path -LiteralPath $p)) { throw "worktree 없음: $b" }
	$origCfg[$p] = [IO.File]::ReadAllBytes($p)
}

function RunCase($c, [int]$round, [string]$runDir) {
	$root = Join-Path $WtRoot $c.Build
	$cfgPath = Join-Path $root 'networkLibProject\server.cfg'
	$sec = if ($c.Sec -gt 0 -and $RunSec -ge 15) { $c.Sec } else { $RunSec }
	New-Item -ItemType Directory -Force $runDir | Out-Null
	$sw0 = [Diagnostics.Stopwatch]::StartNew()
	while ((Get-Process networkLibProject -ErrorAction SilentlyContinue) -and $sw0.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 200 }
	$waited = WaitQuiet
	$before = TopOther
	$base = $enc.GetString($origCfg[$cfgPath])
	[IO.File]::WriteAllText($cfgPath, (SetKeys $base $c.Srv), $enc)

	$srv = Start-Process -FilePath "$root\networkLibProject\x64\Release\networkLibProject.exe" -WorkingDirectory "$root\networkLibProject" `
		-RedirectStandardOutput "$runDir\server.txt" -RedirectStandardError "$runDir\server_err.txt" -WindowStyle Hidden -PassThru
	# 서버가 수락 준비를 마친 뒤 클라를 띄운다 - 2초만 기다리면 PC가 바쁠 때 connectFail이 난다
	$swReady = [Diagnostics.Stopwatch]::StartNew()
	while ($swReady.Elapsed.TotalSeconds -lt 20) {
		if (Select-String -LiteralPath "$runDir\server.txt" -Pattern 'Ctrl\+C' -SimpleMatch:$false -Encoding Default -Quiet -ErrorAction SilentlyContinue) { break }
		Start-Sleep -Milliseconds 100
	}
	Start-Sleep -Milliseconds 500
	$clis = @()
	$i = 0
	foreach ($cl in $c.Clients) {
		$dir = Join-Path $runDir "client$i"
		New-Item -ItemType Directory -Force $dir | Out-Null
		$cliRoot = if ($cl.Build) { Join-Path $WtRoot $cl.Build } else { $root }	# 클라만 다른 커밋을 쓰는 경우
		$t = $enc.GetString([IO.File]::ReadAllBytes((Join-Path $cliRoot "$($cl.Proj)\$($cl.Cfg)")))
		[IO.File]::WriteAllText((Join-Path $dir 'client.cfg'), (SetKeys $t $cl.Over), $enc)
		$clis += Start-Process -FilePath "$cliRoot\$($cl.Proj)\x64\Release\$($cl.Proj).exe" -WorkingDirectory $dir `
			-RedirectStandardOutput "$runDir\client$i.txt" -RedirectStandardError "$runDir\client${i}_err.txt" -WindowStyle Hidden -PassThru
		$i++
	}
	Start-Sleep -Seconds ([int]($sec / 2))
	$mid = TopOther
	Start-Sleep -Seconds ($sec - [int]($sec / 2))
	foreach ($p in $clis) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
	Start-Sleep -Seconds 3
	Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$root\tools\send_ctrlc.ps1`"", '-TargetPid', $srv.Id, '-LogPath', "`"$runDir\ctrlc.txt`"" -WindowStyle Hidden -Wait | Out-Null
	$exited = $srv.WaitForExit(15000)
	if (-not $exited) { Stop-Process -Id $srv.Id -Force }

	$s = @(Get-Content -LiteralPath "$runDir\server.txt" -Encoding Default)
	$allStat = @($s | Where-Object { $_ -match '^\[stat\]' })
	$stat = Steady @($allStat | Where-Object { $_ -notmatch '세션=0 ' })
	$chat = Steady @($s | Where-Object { $_ -match '^\[chat\]' })
	$last = if ($allStat.Count) { $allStat[-1] } else { '' }
	$pkt = Med (Grab $stat '수신패킷/s=([\d.]+)')
	$deliver = Med (Grab $chat '전달/s=([\d.]+)')
	$cores = Med (Grab $stat 'cpu코어=([\d.]+)/')
	$bytesPerSend = Med (Grab $stat '바이트/송신완료=([\d.]+)')
	$pktPerSend = Med (Grab $stat '패킷/송신완료=([\d.]+)')
	if ($pktPerSend -eq 0 -and $c.MsgSize -gt 0) { $pktPerSend = [math]::Round($bytesPerSend / $c.MsgSize, 2) }
	$rate = if ($deliver -gt 0) { $deliver } else { $pkt }
	$row = [ordered]@{
		Stage = $c.Stage; Build = $c.Build; Label = $c.Label; Workers = $c.Workers; Round = $round; MsgSize = $c.MsgSize; Room = $c.Room
		PktPerSec = $pkt; DeliverPerSec = $deliver; MBps = [math]::Round($pkt * $c.MsgSize / 1e6, 1)
		PktPerSend = $pktPerSend; BytesPerSend = $bytesPerSend; CompletePerGqcs = (Med (Grab $stat '완료/GQCS=([\d.]+)'))
		ServerCores = $cores; KernelPct = (Med (Grab $stat '커널 ([\d.]+)%'))
		CpuUsPerUnit = $(if ($rate -gt 0) { [math]::Round($cores * 1e6 / $rate, 3) } else { 0 })
		SendWaitUs = (Med (Grab $stat '송신완료까지\(us\)=([\d.]+)')); SendSyncPct = (Med (Grab $stat '송신즉시=([\d.]+)%'))
		LockHoldUs = (Med (Grab $chat '방락보유\(us\)=([\d.]+)')); RttP50Us = (Med (Grab $stat 'p50=(\d+)'))
		Overflow = [regex]::Match($last, '넘침끊김=(\d+)').Groups[1].Value; EndSession = [regex]::Match($last, '세션=(\d+)').Groups[1].Value
		StatLines = $allStat.Count; Exited = $exited; IdleCut = [regex]::Match((@($s | Where-Object { $_ -match '^\[chat\]' } | Select-Object -Last 1) -join ''), '무응답끊김=(\d+)').Groups[1].Value
	}
	for ($j = 0; $j -lt 2; $j++) {
		$f = "$runDir\client$j.txt"
		$cLines = if (Test-Path -LiteralPath $f) { @(Get-Content -LiteralPath $f -Encoding Default | Where-Object { $_ -match 'unexpectedClose=' }) } else { @() }
		$st = Steady $cLines
		$row["C${j}Cores"] = Med (Grab $st 'cpu코어=([\d.]+)/')
		$row["C${j}Close"] = if ($cLines.Count) { [regex]::Match($cLines[-1], 'unexpectedClose=(\d+)').Groups[1].Value } else { '' }
		$row["C${j}ConnFail"] = if ($cLines.Count) { [regex]::Match($cLines[-1], 'connectFail=(\d+)').Groups[1].Value } else { '' }
		$row["C${j}MsgPerSec"] = Med (Grab $st '[^신]메시지/s=(\d+)')
		$row["C${j}LatP50Us"] = Med (Grab $st 'p50=(\d+)')
		$row["C${j}LatP99Us"] = Med (Grab $st 'p99=(\d+)')
		$mx = @(Grab $cLines 'max=(\d+)') | Measure-Object -Maximum
		$row["C${j}LatMaxUs"] = if ($mx.Count) { $mx.Maximum } else { 0 }
		$row["C${j}ErrBytes"] = if (Test-Path -LiteralPath "$runDir\client${j}_err.txt") { (Get-Item -LiteralPath "$runDir\client${j}_err.txt").Length } else { 0 }
	}
	$row.WaitedSec = $waited; $row.TopBefore = $before; $row.TopMid = $mid
	[pscustomobject]$row
}

try {
	foreach ($stage in $Stages) {
		if ($stage -eq 'mixed') { continue }
		$list = @($cases | Where-Object { $_.Stage -eq $stage })
		if ($LimitPerStage -gt 0) { $list = @($list | Select-Object -First $LimitPerStage) }
		Log "단계 $stage 시작: $($list.Count)건 x $Rounds 회"
		for ($r = 1; $r -le $Rounds; $r++) {
			$shift = [math]::Floor($list.Count * ($r - 1) / $Rounds)
			$order = @($list[$shift..($list.Count - 1)]) + @(if ($shift -gt 0) { $list[0..($shift - 1)] })
			if ($r % 2 -eq 0) { [array]::Reverse($order) }	# 짝수 회차는 뒤집어 번갈아 재는 두 빌드의 앞뒤도 바꾼다
			$n = 0
			foreach ($c in $order) {
				$n++
				# 무효 판정: 처리량 0, 시작 연결 실패, 통계 줄 부족(README_ERRORS 1-16), 측정 도중 다른 프로세스가 코어 절반 이상 - 무효면 한 번 더 잰다. 두 행 모두 CSV에 남긴다
				for ($attempt = 1; $attempt -le 2; $attempt++) {
					$runDir = Join-Path $OutRoot ("{0}\{1}_{2}_w{3}_{4}_r{5}_a{6}" -f $Tag, $c.Stage, $c.Build, $c.Workers, ($c.Label -replace '\s', '_'), $r, $attempt)
					$row = RunCase $c $r $runDir
					$why = @()
					if ($row.PktPerSec -le 0) { $why += 'zero' }
					if ([int]('0' + $row.C0ConnFail) -gt 0) { $why += 'connectFail' }
					$minLines = $(if ($c.Sec -gt 0 -and $RunSec -ge 15) { $c.Sec } else { $RunSec })
					if ($row.StatLines -lt $minLines) { $why += 'statLines' }
					foreach ($m in [regex]::Matches([string]$row.TopMid, '([^\s:]+):(\d+)')) {
						if ($m.Groups[1].Value -notmatch '^(powershell|WmiPrvSE)' -and [int]$m.Groups[2].Value -ge 50) { $why += "busy:$($m.Groups[1].Value)" }
					}
					$row | Add-Member Attempt $attempt
					$row | Add-Member Valid ($why.Count -eq 0)
					$row | Add-Member Invalid ($why -join ',')
					$row | Export-Csv -LiteralPath $csv -Append -NoTypeInformation -Encoding UTF8
					Log ("{0} r{1} {2}/{3} {4} w{5} {6} a{7}: 패킷 {8} 전달 {9} WSASend당 {10} 코어 {11} 넘침 {12} 끊김 {13} 통계 {14} 대기 {15}s 무효[{16}] 위 [{17}] 중 [{18}]" -f $c.Stage, $r, $n, $order.Count, $c.Build, $c.Workers, $c.Label, $attempt, $row.PktPerSec, $row.DeliverPerSec, $row.PktPerSend, $row.ServerCores, $row.Overflow, $row.C0Close, $row.StatLines, $row.WaitedSec, $row.Invalid, $row.TopBefore, $row.TopMid)
					if ($why.Count -eq 0) { break }
				}
			}
		}
	}
}
finally {
	foreach ($p in $origCfg.Keys) { [IO.File]::WriteAllBytes($p, $origCfg[$p]) }
}
if ($Stages -contains 'mixed') {
	Log '섞인 클라 합격 시험 시작'
	$w = Join-Path $WtRoot $head
	& (Join-Path $w 'tools\mixed_test.ps1') -Rounds $Rounds -Tag "remeasure_$Tag" 2>&1 | Out-File -LiteralPath (Join-Path $OutRoot "${Tag}_mixed.txt") -Encoding UTF8
	Copy-Item (Join-Path $env:TEMP "arkchimy_echo_test\mixed_remeasure_$Tag.csv") (Join-Path $OutRoot "${Tag}_mixed_runs.csv") -ErrorAction SilentlyContinue
}
Log '전체 끝'
