$repo = 'C:\_ArkchimyProject'
Set-Location $repo
$root = Join-Path $env:TEMP 'arkchimy_echo_test'
$base = 'C:\_Arkchimy_deadlock'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild "$base\networkLibProject\networkLibProject.sln" /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo | Select-String -Pattern 'error |networkLibProject.exe' | ForEach-Object { $_.Line }
$baseExe = "$base\networkLibProject\x64\Release\networkLibProject.exe"
foreach ($r in 1..4) {
    foreach ($b in 'base', 'fix') {
        $name = "norecv_${b}_r$r"
        if ($b -eq 'base') {
            & .\tools\echo_test.ps1 -Name $name -RunSec 15 -ClientCfgs client.cfg,client_noRecv.cfg -ServerExe $baseExe | Out-Null
        } else {
            & .\tools\echo_test.ps1 -Name $name -RunSec 15 -ClientCfgs client.cfg,client_noRecv.cfg | Out-Null
        }
        $d = Join-Path $root $name
        $last = @(Get-Content "$d\server.txt" -Encoding Default | Where-Object { $_ -like '`[stat`]*' })
        $over = if ($last[-1] -match '넘침끊김=(\d+)') { $Matches[1] } else { '?' }
        $c0 = Get-Content "$d\client0.txt" -Encoding Default | Where-Object { $_ -match 'unexpectedClose=' } | Select-Object -Last 1
        $c1 = Get-Content "$d\client1.txt" -Encoding Default | Where-Object { $_ -match 'unexpectedClose=' } | Select-Object -Last 1
        $n0 = if ($c0 -match 'unexpectedClose=(\d+)') { $Matches[1] } else { '?' }
        $n1 = if ($c1 -match 'unexpectedClose=(\d+)') { $Matches[1] } else { '?' }
        "$b r$r 넘침=$over 정상끊김=$n0 받지않는클라끊김=$n1 통계줄=$($last.Count)"
    }
}
"done"
