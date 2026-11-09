param([int]$Rounds = 3, [int]$RunSec = 20, [string]$Wt, [int[]]$Workers = @(2, 4), [string]$OutName = 'arkchimy_prof_pcb')
# base: profiler off (throughput). coarse/fine: profiler on, window 5-15s. Alternates variants each round.
$wt = $Wt
$outRoot = Join-Path $env:TEMP $OutName
$cfgPath = "$wt\networkLibProject\server.cfg"
$noBom = New-Object Text.UTF8Encoding($false)
$cfgBase = [IO.File]::ReadAllText("D:\_Arkchimy\networkLibProject\server.cfg")
New-Item -ItemType Directory -Force $outRoot | Out-Null
$orders = @(@('base', 'coarse'), @('coarse', 'base'), @('base', 'coarse'))
foreach ($r in 1..$Rounds) {
    foreach ($w in $Workers) {
        foreach ($v in $orders[($r - 1) % 3]) {
            $profile = if ($v -eq 'base') { 0 } else { 1 }
            $cfg = $cfgBase -creplace 'chatRoomCnt = \d+;', 'chatRoomCnt = 16;' -creplace 'workerCnt = \d+;', "workerCnt = $w;" -creplace 'profile = \d+;', "profile = $profile;"
            [IO.File]::WriteAllText($cfgPath, $cfg, $noBom)
            Get-ChildItem "$wt\networkLibProject\*_Profiler.txt" -ErrorAction SilentlyContinue | Remove-Item -Force
            $top = (Get-CimInstance Win32_PerfFormattedData_PerfProc_Process | Where-Object { $_.Name -notmatch '_Total|Idle' } | Sort-Object PercentProcessorTime -Descending | Select-Object -First 3 | ForEach-Object { "$($_.Name)=$($_.PercentProcessorTime)" }) -join ' '
            $name = "${v}_w${w}_r$r"
            $sw = [Diagnostics.Stopwatch]::StartNew()
            while ((Get-Process networkLibProject -ErrorAction SilentlyContinue) -and $sw.Elapsed.TotalSeconds -lt 15) { Start-Sleep -Milliseconds 200 }
            try { & "$wt\tools\echo_test.ps1" -Name $name -RunSec $RunSec -OutRoot $outRoot -ClientCfgs 'LoadClient/client_chat.cfg' -ServerExe "$wt\prof_bin\$v\networkLibProject.exe" | Out-Null }
            catch { "[재시도] $name : $_"; Get-Process networkLibProject, LoadClient -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 3; & "$wt\tools\echo_test.ps1" -Name $name -RunSec $RunSec -OutRoot $outRoot -ClientCfgs 'LoadClient/client_chat.cfg' -ServerExe "$wt\prof_bin\$v\networkLibProject.exe" | Out-Null }
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
