param(
    [string[]]$Conditions = @("1:1", "2:1", "4:1", "8:1", "8:2"),	# "워커수:LoadClient수[:모드[:크기x개수]]" - 모드: gqcs(기본) 또는 ex<N>(GQCSEx, 한 번에 최대 N개), 크기x개수: LoadClient msgSize x overSend(생략하면 client.cfg 그대로). 탐침은 항상 1개 함께
    [int]$Rounds = 3,
    [int]$RunSec = 15,
    [string]$OutRoot = (Join-Path $env:TEMP "arkchimy_echo_test"),
    [string]$Tag = (Get-Date -Format "yyyyMMdd_HHmmss"),
    [switch]$Profile,	# 켜면 서버 프로파일러를 켜고(server.cfg profile = 1) 패킷 1개당 GQCS/WSARecv/WSASend/로직 비용을 계산한다
    [string]$BaseServerExe = ""	# 모드에 +base를 붙인 조건은 이 서버로 실행 - 수정 전 빌드와 같은 실행 안에서 번갈아 비교할 때
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
function Summarize($dir, [int]$workers, [int]$loadClients, [int]$round, [string]$mode, [int]$msgSize, [int]$overSend) {
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
        Mode = $mode
        MsgSize = $msgSize
        OverSend = $overSend
        MBps = [math]::Round($pktPerSec * $msgSize / 1e6, 1)	# 서버가 받은 메시지 바이트(헤더 포함) - 크기가 다르면 패킷/s로는 비교가 안 된다
        GqcsPerSec = [int](Med (Grab $srv 'GQCS/s=(\d+)'))
        SendSyncPct = Med (Grab $srv '송신즉시=([\d.]+)%')	# WSASend가 즉시 성공(0 반환)한 비율
        SendSyncUs = Med (Grab $srv '송신반환\(us\) 즉시=([\d.]+)')
        SendPendingUs = Med (Grab $srv '송신반환\(us\) 즉시=[\d.]+ 대기=([\d.]+)')
        RecvSyncPct = Med (Grab $srv '수신즉시=([\d.]+)%')
        RecvSyncUs = Med (Grab $srv '수신반환\(us\) 즉시=([\d.]+)')
        RecvPendingUs = Med (Grab $srv '수신반환\(us\) 즉시=[\d.]+ 대기=([\d.]+)')
        SendLatencyUs = Med (Grab $srv '송신완료까지\(us\)=([\d.]+)')	# WSASend를 건 순간부터 그 송신 완료를 처리할 때까지
        CompletePerGqcs = Med (Grab $srv '완료/GQCS=([\d.]+)')	# GQCS 호출 1번에 실제로 꺼낸 완료 수
        PktPerSec = [int]$pktPerSec
        PktPerRecv = Med (Grab $srv '패킷/수신완료=([\d.]+)')	# 수신 완료 1건에 실린 패킷 수
        BytesPerSend = Med (Grab $srv '바이트/송신완료=(\d+)')	# 송신 완료 1건에 실린 바이트
        CompletePerPkt = if ($pktPerSec -gt 0) { [math]::Round($completePerSec / $pktPerSec, 3) } else { 0 }	# 패킷당 완료 수(= GQCS + WSARecv/WSASend 쌍)
        IocpPerPkt = if ($pktPerSec -gt 0) { [math]::Round((Med (Grab $srv '큐완료/s=(\d+)')) / $pktPerSec, 3) } else { 0 }	# 패킷당 IOCP 큐에서 꺼낸 완료 수
        CpuUsPerPkt = if ($pktPerSec -gt 0) { [math]::Round(1000.0 * (Med (Grab $srv 'cpu코어=([\d.]+)/')) / $pktPerSec * 1000, 2) } else { 0 }	# 패킷 1개당 서버 CPU(µs) - 클라가 포화라 처리량이 안 올라도 서버가 가벼워졌는지 본다
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

# 서버 프로파일 파일(Profiler.txt)을 읽어 태그별 (횟수, 총시간 us)를 모든 스레드에 걸쳐 합친다
# 줄 형식: | Tag | Count | Avg(us) | Min | Max | MaxSeq |
function Read-ProfileTotals($file) {
    $totals = @{}
    foreach ($line in Get-Content $file -Encoding Default) {
        if ($line -match '^\|\s*(\S+)\s*\|\s*(\d+)\s*\|\s*([\d.]+)us') {
            $tagName = $Matches[1]; $count = [double]$Matches[2]; $avg = [double]$Matches[3]
            if (-not $totals.ContainsKey($tagName)) { $totals[$tagName] = @{ Count = 0.0; TotalUs = 0.0 } }
            $totals[$tagName].Count += $count
            $totals[$tagName].TotalUs += $count * $avg
        }
    }
    return $totals
}

# 프로파일 결과 → 패킷 1개당 비용(ns)과 호출 수
# 로직 = Complete - WSARecv - WSASend (완료 처리 안에서 부른 시스템 콜을 뺀 라이브러리 코드 시간. 핑 스레드의 WSASend도 빠지지만 작다)
# 패킷당 총비용 = 워커 수 × 구간 길이 ÷ 패킷 수 (워커가 포화일 때 이 값이 곧 처리량의 역수)
function Profile-PerPacket($dir, $serverLines) {
    $info = ($serverLines | Where-Object { $_ -match '\[profile\] 저장=1 구간=([\d.]+)s 패킷=(\d+)' } | Select-Object -Last 1)
    $file = Get-ChildItem $dir -Filter "*_Profiler.txt" | Select-Object -First 1
    if (-not $info -or -not $file) { return $null }
    $null = $info -match '구간=([\d.]+)s 패킷=(\d+) .*워커=(\d+)'
    $windowSec = [double]$Matches[1]; $packets = [double]$Matches[2]; $workers = [double]$Matches[3]
    $t = Read-ProfileTotals $file.FullName
    function NsPer($name) { if ($t.ContainsKey($name)) { 1000.0 * $t[$name].TotalUs / $packets } else { 0 } }
    function CallsPer($name) { if ($t.ContainsKey($name)) { $t[$name].Count / $packets } else { 0 } }
    [pscustomobject]@{
        TotalNsPerPkt = [math]::Round($workers * $windowSec * 1e9 / $packets, 1)
        GqcsNsPerPkt = [math]::Round((NsPer "GQCS"), 1)
        RecvNsPerPkt = [math]::Round((NsPer "WSARecv"), 1)
        SendNsPerPkt = [math]::Round((NsPer "WSASend"), 1)
        LogicNsPerPkt = [math]::Round((NsPer "Complete") - (NsPer "WSARecv") - (NsPer "WSASend"), 1)
        GqcsCallsPerPkt = [math]::Round((CallsPer "GQCS"), 4)
        RecvCallsPerPkt = [math]::Round((CallsPer "WSARecv"), 4)
        SendCallsPerPkt = [math]::Round((CallsPer "WSASend"), 4)
        GqcsNsPerCall = $(if ($t.ContainsKey("GQCS") -and $t["GQCS"].Count -gt 0) { [math]::Round(1000.0 * $t["GQCS"].TotalUs / $t["GQCS"].Count, 1) } else { 0 })
        SendNsPerCall = $(if ($t.ContainsKey("WSASend") -and $t["WSASend"].Count -gt 0) { [math]::Round(1000.0 * $t["WSASend"].TotalUs / $t["WSASend"].Count, 1) } else { 0 })
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
$lcCfgPath = Join-Path $root "LoadClient\client.cfg"
$lcOrig = [IO.File]::ReadAllBytes($lcCfgPath)
$lcText = [Text.Encoding]::UTF8.GetString($lcOrig)
$defMsg = if ($lcText -match 'msgSize = (\d+);') { [int]$Matches[1] } else { 32 }
$defOver = if ($lcText -match 'overSend = (\d+);') { [int]$Matches[1] } else { 16 }
$rows = @()
try {
    for ($round = 1; $round -le $Rounds; $round++) {
        $shift = ($round - 1) % $Conditions.Count
        $order = @($Conditions[$shift..($Conditions.Count - 1)]) + @(if ($shift -gt 0) { $Conditions[0..($shift - 1)] })
        foreach ($cond in $order) {
            $parts = $cond.Split(":")
            $workers = [int]$parts[0]
            $loadClients = [int]$parts[1]
            $mode = if ($parts.Count -ge 3) { $parts[2] } else { "gqcs" }
            # 모드 → server.cfg: gqcs면 gqcsEx = 0, ex<N>이면 gqcsEx = 1, gqcsBatch = N
            # 뒤에 붙이는 선택: +snd0 송신 버퍼 0(sendBufZero = 1), +base 수정 전 서버(-BaseServerExe). 예: ex64+snd0, gqcs+base
            $gqcsEx = 0
            $gqcsBatch = 64
            $sendBufZero = 0
            $serverExe = ""
            $modeParts = $mode.Split("+")
            $baseMode = $modeParts[0]
            foreach ($opt in @($modeParts | Select-Object -Skip 1)) {
                if ($opt -eq "snd0") { $sendBufZero = 1 }
                elseif ($opt -eq "base") {
                    if (-not $BaseServerExe) { throw "+base에는 -BaseServerExe가 필요하다 ($mode)" }
                    $serverExe = $BaseServerExe
                }
                else { throw "모르는 선택: +$opt ($mode)" }
            }
            if ($baseMode -match '^ex(\d+)$') { $gqcsEx = 1; $gqcsBatch = [int]$Matches[1] }
            elseif ($baseMode -ne "gqcs") { throw "모드는 gqcs 또는 ex<N> (+snd0 선택): $mode" }

            # cfg 파서는 BOM을 못 읽는다 - BOM 없이 쓴다
            $profileFlag = if ($Profile) { 1 } else { 0 }
            $text = [Text.Encoding]::UTF8.GetString($orig) -replace 'workerCnt = \d+;', "workerCnt = $workers;" `
                -replace 'gqcsEx = \d+;', "gqcsEx = $gqcsEx;" -replace 'gqcsBatch = \d+;', "gqcsBatch = $gqcsBatch;" `
                -replace 'profile = \d+;', "profile = $profileFlag;" -replace 'sendBufZero = \d+;', "sendBufZero = $sendBufZero;"
            [IO.File]::WriteAllText($cfgPath, $text, (New-Object Text.UTF8Encoding($false)))

            # 메시지 크기 x 묶음 개수 → LoadClient client.cfg (BOM 없이)
            $msgSize = $defMsg
            $overSend = $defOver
            $sizeTag = ""
            if ($parts.Count -ge 4) {
                if ($parts[3] -notmatch '^(\d+)x(\d+)$') { throw "크기x개수 형식 오류: $cond" }
                $msgSize = [int]$Matches[1]; $overSend = [int]$Matches[2]; $sizeTag = "_m${msgSize}x${overSend}"
            }
            $lcNew = $lcText -replace 'msgSize = \d+;', "msgSize = $msgSize;" -replace 'overSend = \d+;', "overSend = $overSend;"
            [IO.File]::WriteAllText($lcCfgPath, $lcNew, (New-Object Text.UTF8Encoding($false)))

            $name = "sweep_${Tag}_r${round}_w${workers}_c${loadClients}_${mode}${sizeTag}"
            $cfgs = @(1..$loadClients | ForEach-Object { "LoadClient/client.cfg" }) + @("client_probe.cfg")
            $clockJob = Start-Job -ScriptBlock $clockSampler -ArgumentList $RunSec
            & (Join-Path $root "tools\echo_test.ps1") -Name $name -RunSec $RunSec -ClientCfgs $cfgs -OutRoot $OutRoot -ServerExe $serverExe | Out-Null
            $clock = @(Receive-Job $clockJob -Wait -AutoRemoveJob)

            # 서버는 작업 폴더(networkLibProject)에 프로파일 파일을 남긴다 - 이번 실행 결과 폴더로 옮긴다
            $runDir = Join-Path $OutRoot $name
            Get-ChildItem (Join-Path $root "networkLibProject") -Filter "networkLibProject_*_Profiler.txt" -ErrorAction SilentlyContinue |
                ForEach-Object { Move-Item $_.FullName $runDir -Force }

            $row = Summarize $runDir $workers $loadClients $round $mode $msgSize $overSend
            if ($Profile) {
                $perPkt = Profile-PerPacket $runDir @(Get-Content "$runDir\server.txt" -Encoding Default)
                if ($perPkt) { $perPkt.PSObject.Properties | ForEach-Object { $row | Add-Member $_.Name $_.Value } }
            }
            $row | Add-Member BusyClockMHz ([int](Med @($clock | ForEach-Object { $_.Busy })))
            $row | Add-Member PClockMHz ([int](Med @($clock | ForEach-Object { $_.P })))
            $rows += $row
            $row | Export-Csv -Path $csv -Append -NoTypeInformation -Encoding UTF8
            "[{0}] 회차 {1} 워커 {2} 클라 {3} {12} {14}Bx{15}: {4} 패킷/s ({16}MB/s), 서버 {5}코어, 클라 {6}코어, 완료/GQCS {13}, 패킷/수신 {7}, B/송신 {8}, 클럭 {9}MHz(P {10}), 탐침 p50 {11}us" -f (Get-Date -Format "HH:mm:ss"), $round, $workers, $loadClients, $row.PktPerSec, $row.ServerCores, $row.ClientCores, $row.PktPerRecv, $row.BytesPerSend, $row.BusyClockMHz, $row.PClockMHz, $row.ProbeP50us, $mode, $row.CompletePerGqcs, $msgSize, $overSend, $row.MBps
        }
    }
}
finally {
    [IO.File]::WriteAllBytes($cfgPath, $orig)	# 중간에 멈춰도 server.cfg, client.cfg는 원래대로
    [IO.File]::WriteAllBytes($lcCfgPath, $lcOrig)
}

"`n=== 조건별 중앙값 ($Rounds 회) - CSV: $csv ==="
$rows | Group-Object Workers, LoadClients, Mode, MsgSize, OverSend | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        워커 = $g[0].Workers
        클라 = $g[0].LoadClients
        모드 = $g[0].Mode
        크기 = "{0}x{1}" -f $g[0].MsgSize, $g[0].OverSend
        'GQCS/s' = [int](Med $g.GqcsPerSec)
        '완료/GQCS' = Med $g.CompletePerGqcs
        '패킷/s' = [int](Med $g.PktPerSec)
        'MB/s' = Med $g.MBps
        '패킷/s 범위' = "{0}~{1}" -f ($g.PktPerSec | Measure-Object -Minimum).Minimum, ($g.PktPerSec | Measure-Object -Maximum).Maximum
        서버코어 = Med $g.ServerCores
        '패킷/코어' = [int]((Med $g.PktPerSec) / [math]::Max(0.01, (Med $g.ServerCores)))
        클라코어 = "{0}/{1}" -f (Med $g.ClientCores), $g[0].ClientThreads	# 사용 코어 / LoadClient 워커 스레드 합 (클라 여러 개면 합)
        '패킷/수신' = Med $g.PktPerRecv
        'B/송신' = Med $g.BytesPerSend
        '완료/패킷' = Med $g.CompletePerPkt
        '큐완료/패킷' = Med $g.IocpPerPkt
        'CPU us/패킷' = Med $g.CpuUsPerPkt
        '클럭MHz' = Med $g.BusyClockMHz
        'P클럭MHz' = Med $g.PClockMHz
        '탐침p50(ms)' = [math]::Round((Med $g.ProbeP50us) / 1000, 1)
        '탐침p99(ms)' = [math]::Round((Med $g.ProbeP99us) / 1000, 1)
        '부하p50(ms)' = [math]::Round((Med $g.LoadP50us) / 1000, 1)
        '부하p99(ms)' = [math]::Round((Med $g.LoadP99us) / 1000, 1)
        오류 = "close={0} 넘침={1}" -f ($g.UnexpectedClose | Measure-Object -Sum).Sum, ($g.Overflow | Measure-Object -Maximum).Maximum
    }
} | Sort-Object 워커, 클라, 크기, 모드 | Format-Table -Property * -AutoSize | Out-String -Width 500	# 열이 10개를 넘으면 -Property * 없이는 일부가 숨는다

"`n=== WSASend / WSARecv 반환 결과 (조건별 중앙값) ==="
$rows | Group-Object Workers, LoadClients, Mode, MsgSize, OverSend | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        워커 = $g[0].Workers
        모드 = $g[0].Mode
        크기 = "{0}x{1}" -f $g[0].MsgSize, $g[0].OverSend
        '패킷/s' = [int](Med $g.PktPerSec)
        'MB/s' = Med $g.MBps
        '송신 즉시성공%' = Med $g.SendSyncPct
        '송신 반환(us) 즉시' = Med $g.SendSyncUs
        '송신 반환(us) 대기' = Med $g.SendPendingUs
        '송신 완료까지(us)' = Med $g.SendLatencyUs
        '서버코어' = Med $g.ServerCores
        '커널%' = Med $g.KernelPct
        '탐침p50(ms)' = [math]::Round((Med $g.ProbeP50us) / 1000, 1)
        '부하p50(ms)' = [math]::Round((Med $g.LoadP50us) / 1000, 1)
        '수신 즉시성공%' = Med $g.RecvSyncPct
        '수신 반환(us) 즉시' = Med $g.RecvSyncUs
        '수신 반환(us) 대기' = Med $g.RecvPendingUs
        'B/송신' = Med $g.BytesPerSend
    }
} | Sort-Object 워커, 크기, 모드 | Format-Table -Property * -AutoSize | Out-String -Width 400

if ($Profile) {
    "`n=== 패킷 1개당 비용 (ns, 프로파일 구간, 조건별 중앙값) ==="
    $rows | Where-Object { $_.PSObject.Properties.Name -contains 'TotalNsPerPkt' } | Group-Object Workers, LoadClients, Mode, MsgSize, OverSend | ForEach-Object {
        $g = $_.Group
        [pscustomobject]@{
            워커 = $g[0].Workers
            모드 = $g[0].Mode
            크기 = "{0}x{1}" -f $g[0].MsgSize, $g[0].OverSend
            '총비용' = Med $g.TotalNsPerPkt
            'GQCS' = Med $g.GqcsNsPerPkt
            'WSARecv' = Med $g.RecvNsPerPkt
            'WSASend' = Med $g.SendNsPerPkt
            '로직' = Med $g.LogicNsPerPkt
            '나머지' = [math]::Round((Med $g.TotalNsPerPkt) - (Med $g.GqcsNsPerPkt) - (Med $g.RecvNsPerPkt) - (Med $g.SendNsPerPkt) - (Med $g.LogicNsPerPkt), 1)
            'GQCS호출/패킷' = Med $g.GqcsCallsPerPkt
            'Recv호출/패킷' = Med $g.RecvCallsPerPkt
            'Send호출/패킷' = Med $g.SendCallsPerPkt
            'GQCS 1회(ns)' = Med $g.GqcsNsPerCall
            'Send 1회(ns)' = Med $g.SendNsPerCall
        }
    } | Sort-Object 워커, 크기, 모드 | Format-Table -Property * -AutoSize | Out-String -Width 500
}
