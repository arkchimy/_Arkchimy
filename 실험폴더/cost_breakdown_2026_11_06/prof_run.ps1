param([int]$Rounds = 3, [int]$RunSec = 20)
# base: profiler off (throughput). coarse/fine: profiler on, window 5-15s. Alternates variants each round.
$wt = 'C:\_Arkchimy_prof'
$outRoot = Join-Path $env:TEMP 'arkchimy_prof'
$cfgPath = "$wt\networkLibProject\server.cfg"
$noBom = New-Object Text.UTF8Encoding($false)
$cfgBase = [IO.File]::ReadAllText("C:\_ArkchimyProject\networkLibProject\server.cfg")
New-Item -ItemType Directory -Force $outRoot | Out-Null
$orders = @(@('base', 'coarse', 'fine'), @('fine', 'base', 'coarse'), @('coarse', 'fine', 'base'))
foreach ($r in 1..$Rounds) {
    foreach ($w in 2, 4) {
        foreach ($v in $orders[($r - 1) % 3]) {
            $profile = if ($v -eq 'base') { 0 } else { 1 }
            $cfg = $cfgBase -creplace 'chatRoomCnt = \d+;', 'chatRoomCnt = 16;' -creplace 'workerCnt = \d+;', "workerCnt = $w;" -creplace 'profile = \d+;', "profile = $profile;"
            [IO.File]::WriteAllText($cfgPath, $cfg, $noBom)
            Get-ChildItem "$wt\networkLibProject\*_Profiler.txt" -ErrorAction SilentlyContinue | Remove-Item -Force
            $top = (Get-CimInstance Win32_PerfFormattedData_PerfProc_Process | Where-Object { $_.Name -notmatch '_Total|Idle' } | Sort-Object PercentProcessorTime -Descending | Select-Object -First 3 | ForEach-Object { "$($_.Name)=$($_.PercentProcessorTime)" }) -join ' '
            $name = "${v}_w${w}_r$r"
            & "$wt\tools\echo_test.ps1" -Name $name -RunSec $RunSec -OutRoot $outRoot -ClientCfgs 'LoadClient/client_chat.cfg' -ServerExe "$wt\prof_bin\$v\networkLibProject.exe" | Out-Null
            $prof = Get-ChildItem "$wt\networkLibProject\*_Profiler.txt" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
            if ($prof) { Copy-Item $prof.FullName (Join-Path "$outRoot\$name" 'profile.txt') -Force }
            $top | Out-File (Join-Path "$outRoot\$name" 'top.txt') -Encoding utf8
            "$name 프로파일=$([bool]$prof) 다른프로세스: $top"
            Get-Process networkLibProject, LoadClient -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
        }
    }
}
"done"
