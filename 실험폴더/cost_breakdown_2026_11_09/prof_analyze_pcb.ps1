param([string]$OutName = 'arkchimy_prof_pcb')
$root = Join-Path $env:TEMP $OutName
$rows = @()
$tagRows = @()
foreach ($d in Get-ChildItem $root -Directory | Sort-Object Name) {
    if ($d.Name -notmatch '^(base|coarse|fine)_w(\d)_r(\d)$') { continue }
    $v = $Matches[1]; $w = [int]$Matches[2]; $r = [int]$Matches[3]
    $lines = @(Get-Content "$($d.FullName)\server.txt" -Encoding Default)
    $stats = @($lines | Where-Object { $_ -like '`[stat`]*' })
    $chats = @($lines | Where-Object { $_ -like '`[chat`]*' })
    # window ticks 6..15 (same as profile window), index = tick-1
    $deliver = @(); $wsa = @(); $cpu = @(); $kern = @(); $batch = @(); $qdone = @(); $hold = @()
    for ($i = 5; $i -lt [Math]::Min(15, $stats.Count); $i++) {
        if ($chats[$i] -match '전달/s=(\d+)') { $deliver += [double]$Matches[1] }
        if ($chats[$i] -match '방락보유\(us\)=([\d.]+)') { $hold += [double]$Matches[1] }
        if ($stats[$i] -match '패킷/송신완료=([\d.]+)') { $wsa += [double]$Matches[1] }
        if ($stats[$i] -match 'cpu코어=([\d.]+)/\d+ \(커널 ([\d.]+)%\)') { $cpu += [double]$Matches[1]; $kern += [double]$Matches[2] }
        if ($stats[$i] -match '완료/GQCS=([\d.]+)') { $batch += [double]$Matches[1] }
    }
    $avg = { param($a) if ($a.Count) { ($a | Measure-Object -Average).Average } else { 0 } }
    $profLine = @($lines | Where-Object { $_ -like '`[profile`]*' }) | Select-Object -Last 1
    $sec = 0.0; if ($profLine -match '구간=([\d.]+)s') { $sec = [double]$Matches[1] }
    $dps = & $avg $deliver
    $rows += [pscustomobject]@{ V = $v; W = $w; R = $r; Stat = $stats.Count; Deliver = [math]::Round($dps); WsaMsg = [math]::Round((& $avg $wsa), 1); Cpu = [math]::Round((& $avg $cpu), 2); Kern = [math]::Round((& $avg $kern), 0); Batch = [math]::Round((& $avg $batch), 1); Hold = [math]::Round((& $avg $hold), 2); Close = ((Get-Content "$($d.FullName)\client0.txt" -Encoding Default | Where-Object { $_ -match 'unexpectedClose=' } | Select-Object -Last 1) -replace '.*unexpectedClose=(\d+).*', '$1'); Done = @($lines | Where-Object { $_ -like '*종료 완료*' }).Count; Top = (Get-Content "$($d.FullName)\top.txt" -Raw).Trim() }
    if (Test-Path "$($d.FullName)\profile.txt") {
        $sum = @{}
        foreach ($l in Get-Content "$($d.FullName)\profile.txt") {
            if ($l -match '^\|\s*(\S+)\s*\|\s*(\d+)\s*\|\s*([\d.]+)us') {
                $tag = $Matches[1]; $cnt = [double]$Matches[2]; $us = [double]$Matches[3]
                if (-not $sum.ContainsKey($tag)) { $sum[$tag] = @{ Cnt = 0.0; Us = 0.0 } }
                $sum[$tag].Cnt += $cnt; $sum[$tag].Us += $cnt * $us
            }
        }
        $deliveries = $dps * $sec
        foreach ($k in $sum.Keys) {
            $tagRows += [pscustomobject]@{ V = $v; W = $w; R = $r; Tag = $k; Cnt = $sum[$k].Cnt; AvgUs = [math]::Round($sum[$k].Us / [math]::Max($sum[$k].Cnt, 1), 3); ShareOfWorkers = [math]::Round(100 * $sum[$k].Us / 1e6 / ($sec * $w), 1); NsPerDeliver = if ($deliveries -gt 0) { [math]::Round(1000 * $sum[$k].Us / $deliveries, 1) } else { 0 }; PerDeliver = if ($deliveries -gt 0) { [math]::Round($sum[$k].Cnt / $deliveries, 4) } else { 0 } }
        }
    }
}
$rows | Export-Csv "$root\runs.csv" -NoTypeInformation -Encoding UTF8
$tagRows | Export-Csv "$root\tags.csv" -NoTypeInformation -Encoding UTF8
$rows | Sort-Object W, V, R | Format-Table V, W, R, Stat, Deliver, WsaMsg, Batch, Cpu, Kern, Hold, Close -AutoSize | Out-String -Width 180
"--- 다른 프로세스 최대"
$rows | ForEach-Object { $_.Top } | Select-Object -Unique | Select-Object -First 8
$med = { param($a) $s = @($a | Sort-Object); if ($s.Count) { $s[[int][math]::Floor($s.Count / 2)] } else { 0 } }
"--- 태그 중앙값 (ns/전달, 워커 시간 %, 평균 us, 건/전달)"
foreach ($g in ($tagRows | Group-Object V, W, Tag | Sort-Object Name)) {
    $x = $g.Group
    '{0,-28} ns/전달={1,8} 워커%={2,6} avgUs={3,8} 건/전달={4,8} 회차ns={5}' -f $g.Name, (& $med ($x.NsPerDeliver)), (& $med ($x.ShareOfWorkers)), (& $med ($x.AvgUs)), (& $med ($x.PerDeliver)), (($x.NsPerDeliver | Sort-Object) -join '/')
}
