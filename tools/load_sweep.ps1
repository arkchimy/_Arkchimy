param(
    [string[]]$Conditions = @("1:1", "2:1", "4:1", "8:1", "8:2"),	# "워커수:LoadClient수" - 탐침(client_probe.cfg)은 항상 1개 함께
    [int]$Rounds = 3,
    [int]$RunSec = 15,
    [string]$OutRoot = (Join-Path $env:TEMP "arkchimy_echo_test"),
    [string]$Tag = (Get-Date -Format "yyyyMMdd_HHmmss")
)

# 서버 워커 수와 부하 클라 수를 바꿔 가며 처리량·CPU·탐침 RTT를 잰다
# 회차마다 실행 순서를 한 칸씩 돌려 시간에 따른 변화(발열 등)가 특정 조건에만 쏠리지 않게 한다
# 회차별 결과는 CSV로 남기고, 끝에 조건별 중앙값 표를 출력한다
# 실행 전에 networkLibProject.sln, EchoClient.sln, LoadClient.sln을 Release x64로 빌드해 둘 것
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$csv = Join-Path $OutRoot "load_sweep_$Tag.csv"
New-Item -ItemType Directory -Force $OutRoot | Out-Null

function Med([double[]]$v) { if (-not $v -or $v.Count -eq 0) { return 0 }; $s = $v | Sort-Object; return $s[[math]::Floor($s.Count / 2)] }
function Grab($lines, $pattern) { @($lines | ForEach-Object { if ($_ -match $pattern) { [double]$Matches[1] } }) }
function Steady($lines) { @($lines | Select-Object -Skip 2 | Select-Object -SkipLast 1) }	# 연결 직후 2초와 종료 직전 1초 제외

# 한 번 실행한 결과 폴더에서 1초 값들의 중앙값을 뽑는다
function Summarize($dir, [int]$workers, [int]$loadClients, [int]$round) {
    $srv = Steady @(Get-Content "$dir\server.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' -and $_ -notmatch '세션=0 ' })
    $clientCores = 0.0
    $clientThreads = 0	# LoadClient 워커 스레드 수 합 - 클라 출력 "cpu코어=사용/스레드"에서 읽는다
    $closes = 0
    for ($i = 0; $i -lt $loadClients; $i++) {
        $lc = @(Get-Content "$dir\client$i.txt" -Encoding Default | Where-Object { $_ -match '메시지/s=' })
        $clientCores += Med (Grab (Steady $lc) 'cpu코어=([\d.]+)/')
        $clientThreads += [int]((Grab $lc 'cpu코어=[\d.]+/(\d+)') | Select-Object -First 1)
        $closes += [int]((Grab $lc 'unexpectedClose=(\d+)') | Select-Object -Last 1)
    }
    $pb = Steady @(Get-Content "$dir\client$loadClients.txt" -Encoding Default | Where-Object { $_ -match '탐침 핑' })
    $pktPerSec = Med (Grab $srv '수신패킷/s=(\d+)')
    $completePerSec = Med (Grab $srv '완료/s=(\d+)')
    $overflow = (Grab @(Get-Content "$dir\server.txt" -Encoding Default) '넘침끊김=(\d+)') | Select-Object -Last 1
    [pscustomobject]@{
        Round = $round
        Workers = $workers
        LoadClients = $loadClients
        PktPerSec = [int]$pktPerSec
        PktPerRecv = Med (Grab $srv '패킷/수신완료=([\d.]+)')	# 수신 완료 1건에 실린 패킷 수
        BytesPerSend = Med (Grab $srv '바이트/송신완료=(\d+)')	# 송신 완료 1건에 실린 바이트
        CompletePerPkt = if ($pktPerSec -gt 0) { [math]::Round($completePerSec / $pktPerSec, 3) } else { 0 }	# 패킷당 완료 수(= GQCS + WSARecv/WSASend 쌍)
        ServerCores = [math]::Round((Med (Grab $srv 'cpu코어=([\d.]+)/')), 2)
        KernelPct = Med (Grab $srv '커널 (\d+)%')
        ClientCores = [math]::Round($clientCores, 2)
        ClientThreads = $clientThreads
        ProbeP50us = Med (Grab $pb 'p50=(\d+)')
        ProbeP99us = Med (Grab $pb ' p99=(\d+)')
        LoadP50us = Med (Grab $srv 'rtt\(us\) p50=(\d+)')
        LoadP99us = Med (Grab $srv ' p99=(\d+)')
        UnexpectedClose = $closes
        Overflow = $overflow
    }
}

# 측정 중 1초마다 코어별 실효 클럭(기준 클럭 × 성능%)을 읽는다 - 성능 카운터 이름은 현지화되므로 WMI 클래스를 쓴다
# Busy: 코어 사용률로 가중 평균한 클럭 = 실제로 일이 처리된 클럭, P: 기준 클럭이 가장 높은 코어(성능 코어)들의 평균
# 서버가 뜨고 클라가 붙는 앞부분(약 4초)은 건너뛴다. WMI 조회 자체도 CPU를 조금 쓴다
$clockSampler = {
    param([int]$runSec)
    Start-Sleep -Seconds 4
    for ($i = 0; $i -lt $runSec - 3; $i++) {
        $cores = @(Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation | Where-Object { $_.Name -notlike '*_Total' })
        $maxBase = ($cores | Measure-Object -Property ProcessorFrequency -Maximum).Maximum
        $busySum = 0.0; $busyMhz = 0.0; $pSum = 0.0; $pCnt = 0
        foreach ($c in $cores) {
            $mhz = $c.ProcessorFrequency * $c.PercentProcessorPerformance / 100.0
            $busySum += $c.PercentProcessorTime
            $busyMhz += $mhz * $c.PercentProcessorTime
            if ($c.ProcessorFrequency -eq $maxBase) { $pSum += $mhz; $pCnt++ }
        }
        [pscustomobject]@{ Busy = $(if ($busySum -gt 0) { $busyMhz / $busySum } else { 0 }); P = $(if ($pCnt -gt 0) { $pSum / $pCnt } else { 0 }) }
        Start-Sleep -Seconds 1
    }
}

$cfgPath = Join-Path $root "networkLibProject\server.cfg"
$orig = [IO.File]::ReadAllBytes($cfgPath)
$rows = @()
try {
    for ($round = 1; $round -le $Rounds; $round++) {
        $shift = ($round - 1) % $Conditions.Count
        $order = @($Conditions[$shift..($Conditions.Count - 1)]) + @(if ($shift -gt 0) { $Conditions[0..($shift - 1)] })
        foreach ($cond in $order) {
            $workers, $loadClients = $cond.Split(":") | ForEach-Object { [int]$_ }
            # cfg 파서는 BOM을 못 읽는다 - BOM 없이 쓴다
            $text = [Text.Encoding]::UTF8.GetString($orig) -replace 'workerCnt = \d+;', "workerCnt = $workers;"
            [IO.File]::WriteAllText($cfgPath, $text, (New-Object Text.UTF8Encoding($false)))

            $name = "sweep_${Tag}_r${round}_w${workers}_c${loadClients}"
            $cfgs = @(1..$loadClients | ForEach-Object { "LoadClient/client.cfg" }) + @("client_probe.cfg")
            $clockJob = Start-Job -ScriptBlock $clockSampler -ArgumentList $RunSec
            & (Join-Path $root "tools\echo_test.ps1") -Name $name -RunSec $RunSec -ClientCfgs $cfgs -OutRoot $OutRoot | Out-Null
            $clock = @(Receive-Job $clockJob -Wait -AutoRemoveJob)

            $row = Summarize (Join-Path $OutRoot $name) $workers $loadClients $round
            $row | Add-Member BusyClockMHz ([int](Med @($clock | ForEach-Object { $_.Busy })))
            $row | Add-Member PClockMHz ([int](Med @($clock | ForEach-Object { $_.P })))
            $rows += $row
            $row | Export-Csv -Path $csv -Append -NoTypeInformation -Encoding UTF8
            "[{0}] 회차 {1} 워커 {2} 클라 {3}: {4} 패킷/s, 서버 {5}코어, 클라 {6}코어, 패킷/수신 {7}, B/송신 {8}, 클럭 {9}MHz(P {10}), 탐침 p50 {11}us" -f (Get-Date -Format "HH:mm:ss"), $round, $workers, $loadClients, $row.PktPerSec, $row.ServerCores, $row.ClientCores, $row.PktPerRecv, $row.BytesPerSend, $row.BusyClockMHz, $row.PClockMHz, $row.ProbeP50us
        }
    }
}
finally {
    [IO.File]::WriteAllBytes($cfgPath, $orig)	# 중간에 멈춰도 server.cfg는 원래대로
}

"`n=== 조건별 중앙값 ($Rounds 회) - CSV: $csv ==="
$rows | Group-Object Workers, LoadClients | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        워커 = $g[0].Workers
        클라 = $g[0].LoadClients
        '패킷/s' = [int](Med $g.PktPerSec)
        '패킷/s 범위' = "{0}~{1}" -f ($g.PktPerSec | Measure-Object -Minimum).Minimum, ($g.PktPerSec | Measure-Object -Maximum).Maximum
        서버코어 = Med $g.ServerCores
        '패킷/코어' = [int]((Med $g.PktPerSec) / [math]::Max(0.01, (Med $g.ServerCores)))
        클라코어 = "{0}/{1}" -f (Med $g.ClientCores), $g[0].ClientThreads	# 사용 코어 / LoadClient 워커 스레드 합 (클라 여러 개면 합)
        '패킷/수신' = Med $g.PktPerRecv
        'B/송신' = Med $g.BytesPerSend
        '완료/패킷' = Med $g.CompletePerPkt
        '클럭MHz' = Med $g.BusyClockMHz
        'P클럭MHz' = Med $g.PClockMHz
        '탐침p50(ms)' = [math]::Round((Med $g.ProbeP50us) / 1000, 1)
        '탐침p99(ms)' = [math]::Round((Med $g.ProbeP99us) / 1000, 1)
        '부하p50(ms)' = [math]::Round((Med $g.LoadP50us) / 1000, 1)
        '부하p99(ms)' = [math]::Round((Med $g.LoadP99us) / 1000, 1)
        오류 = "close={0} 넘침={1}" -f ($g.UnexpectedClose | Measure-Object -Sum).Sum, ($g.Overflow | Measure-Object -Maximum).Maximum
    }
} | Sort-Object 워커, 클라 | Format-Table -Property * -AutoSize | Out-String -Width 400	# 열이 10개를 넘으면 -Property * 없이는 일부가 숨는다
