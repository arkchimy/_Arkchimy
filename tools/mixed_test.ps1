param(
	[int]$Rounds = 3,
	[int]$RunSec = 25,
	[string]$Tag = (Get-Date -Format "yyyyMMdd_HHmmss"),
	[int]$Workers = 2
)
# 섞인 클라 + 포화 부하 합격 시험 - "부하가 걸려도 끊김 정책이 정상 클라를 끊지 않는가"를 본다
# 채팅 서버(방 16, 기본값 GQCSEx + skip + mergeSends = 1)에 정상 클라와 문제 클라를 동시에 붙인다
# 판정 기준을 줄여(무응답 5초, 입장 5초, 송신 정체 3초) 한 번이 RunSec 안에 끝나게 한다
# server.cfg는 실행마다 바꿨다가 끝나면(중간에 멈춰도) 원래 바이트로 되돌린다
#
# 클라(순서 = echo_test의 client0..5)
#   0 정상 채팅 256세션, 하트비트 1초      → 끊김 0
#   1 하트비트만 64세션                    → 끊김 0
#   2 입장 후 침묵 64세션                  → 무응답끊김
#   3 입장 안 함 64세션(하트비트 1초)      → 입장시간초과
#   4 받지 않는 채팅 64세션(하트비트 1초)  → 넘침끊김 또는 송신정체끊김
#   5 EchoClient 비정상 헤더               → 잘못된패킷 또는 비정상패킷
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
$outRoot = Join-Path $env:TEMP "arkchimy_echo_test"
$enc = New-Object Text.UTF8Encoding($false)
$cfgPath = Join-Path $root "networkLibProject\server.cfg"
$orig = [IO.File]::ReadAllBytes($cfgPath)

# 정상 채팅 클라에 하트비트를 붙인 임시 cfg - 원본 끝에 줄바꿈이 없어 줄바꿈부터 붙인다(README_ERRORS 1-20)
$hbCfgName = "client_chat_mixed_tmp.cfg"
$hbCfg = Join-Path $root "LoadClient\$hbCfgName"
$chat = $enc.GetString([IO.File]::ReadAllBytes((Join-Path $root "LoadClient\client_chat.cfg"))).TrimEnd() + "`r`nheartbeatSec = 1;`r`n"
[IO.File]::WriteAllText($hbCfg, $chat, $enc)

$clients = @("LoadClient/$hbCfgName", "LoadClient/client_chatIdleHb.cfg", "LoadClient/client_chatIdle.cfg",
	"LoadClient/client_chatNoJoin.cfg", "LoadClient/client_chatNoRecv.cfg", "client_badPacket.cfg")
$labels = @("정상", "하트비트", "침묵", "입장안함", "받지않음", "비정상")

function LastMatch($lines, $pattern)
{
	$v = $null
	foreach ($l in $lines) { if ($l -match $pattern) { $v = $Matches[1] } }
	return $v
}

$rows = @()
try {
	for ($r = 1; $r -le $Rounds; $r++) {
		$t = $enc.GetString($orig) -creplace 'workerCnt = \d+;', "workerCnt = $Workers;" -creplace 'chatRoomCnt = \d+;', 'chatRoomCnt = 16;' `
			-creplace 'chatIdleSec = \d+;', 'chatIdleSec = 5;' -creplace 'chatJoinSec = \d+;', 'chatJoinSec = 5;' -creplace 'sendStallSec = \d+;', 'sendStallSec = 3;'
		[IO.File]::WriteAllText($cfgPath, $t, $enc)
		while (Get-Process networkLibProject -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 200 }	# 이전 서버가 완전히 끝날 때까지(README_ERRORS 1-17)
		$name = "mixed_${Tag}_r$r"
		& (Join-Path $root "tools\echo_test.ps1") -Name $name -RunSec $RunSec -ClientCfgs $clients | Out-Null
		$o = Join-Path $outRoot $name

		$s = @(Get-Content "$o\server.txt" -Encoding Default)	# 서버 로그는 CP949(README_ERRORS 1-13)
		$stat = @($s | Where-Object { $_ -match '^\[stat\]' })
		$chatLines = @($s | Where-Object { $_ -match '^\[chat\] 입장' })
		$row = [ordered]@{
			Round = $r
			SessionEnd = LastMatch $stat '세션=(\d+)'
			Overflow = LastMatch $stat '넘침끊김=(\d+)'
			SendStall = LastMatch $stat '송신정체끊김=(\d+)'
			BadHeader = LastMatch $stat '비정상패킷=(\d+)'
			Idle = LastMatch $chatLines '무응답끊김=(\d+)'
			JoinTimeout = LastMatch $chatLines '입장시간초과=(\d+)'
			BadChat = LastMatch $chatLines '잘못된패킷=(\d+)'
			Deliver = $(	# 전달/s 중앙값 - 클라가 붙기 전·끝난 뒤의 0은 뺀다
				$v = @($chatLines | ForEach-Object { if ($_ -match '전달/s=([\d.]+)') { [double]$Matches[1] } } | Where-Object { $_ -gt 0 } | Sort-Object)
				if ($v.Count -gt 0) { [int]$v[[math]::Floor($v.Count / 2)] } else { 0 })
			StatLines = $stat.Count
			Done = @($s | Where-Object { $_ -match '종료 완료' }).Count
		}
		for ($i = 0; $i -lt $clients.Count; $i++) {
			$c = @(Get-Content "$o\client$i.txt" -Encoding Default -ErrorAction SilentlyContinue)
			$row["Close_$($labels[$i])"] = LastMatch $c 'unexpectedClose=(\d+)'
			if ($i -eq 0) {
				$maxUs = (@($c | ForEach-Object { if ($_ -match 'max=(\d+)') { [double]$Matches[1] } }) | Measure-Object -Maximum).Maximum
				$row["NormalMaxLatMs"] = [math]::Round($maxUs / 1000, 1)
				$row["NormalSetupErr"] = @($c | Where-Object { $_ -match '설정 파일 오류|init 실패' }).Count	# cfg가 깨져 클라가 안 돈 경우(1-20)
			}
		}
		$rows += [pscustomobject]$row
		"[{0}] 회차 {1}: 정상 끊김 {2}, 하트비트 끊김 {3}, 최대 지연 {4}ms | 무응답 {5}, 입장초과 {6}, 넘침 {7}, 정체 {8}, 잘못된패킷 {9}, 비정상헤더 {10} | 세션끝 {11}, 종료 {12}, 통계줄 {13}" -f `
			(Get-Date -Format "HH:mm:ss"), $r, $row.Close_정상, $row.Close_하트비트, $row.NormalMaxLatMs, $row.Idle, $row.JoinTimeout, $row.Overflow, $row.SendStall, $row.BadChat, $row.BadHeader, $row.SessionEnd, $row.Done, $row.StatLines
	}
}
finally {
	[IO.File]::WriteAllBytes($cfgPath, $orig)
	Remove-Item $hbCfg -ErrorAction SilentlyContinue
}
$csv = Join-Path $outRoot "mixed_$Tag.csv"
$rows | Export-Csv -Path $csv -NoTypeInformation -Encoding UTF8
"`n=== 회차별 결과 - CSV: $csv ==="
$rows | Format-Table -Property * -AutoSize | Out-String -Width 400
