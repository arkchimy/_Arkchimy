$repo = 'C:\_ArkchimyProject'
$wt = 'C:\_Arkchimy_deadlock'
$root = Join-Path $env:TEMP 'arkchimy_echo_test'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
$bom = New-Object Text.UTF8Encoding($true)
$noBom = New-Object Text.UTF8Encoding($false)
Copy-Item "$repo\_lib\NetworkLib\NetworkLib.h" "$wt\_lib\NetworkLib\NetworkLib.h" -Force
Set-Location $wt
& $msbuild 'EchoClient\EchoClient.sln' /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo | Select-String -Pattern 'error ' | ForEach-Object { $_.Line }

$cases = @(
    @{ Name = 'release_pqcs'; Old = 'if (!PostQueuedCompletionStatus(mIOCP, 0, reinterpret_cast<ULONG_PTR>(&session), &session.mReleaseOv))'; New = 'if (!PostQueuedCompletionStatus(nullptr, 0, reinterpret_cast<ULONG_PTR>(&session), &session.mReleaseOv))'; Gqcs = 1; Runs = 1; Worker = 2 }
    @{ Name = 'gqcsex'; Old = 'bOk = GetQueuedCompletionStatusEx(mIOCP,'; New = 'bOk = GetQueuedCompletionStatusEx(nullptr,'; Gqcs = 1; Runs = 1; Worker = 2 }
    @{ Name = 'gqcs'; Old = 'bOk = GetQueuedCompletionStatus(mIOCP,'; New = 'bOk = GetQueuedCompletionStatus(nullptr,'; Gqcs = 0; Runs = 1; Worker = 2 }
    @{ Name = 'stop_signal'; Old = 'if (!PostQueuedCompletionStatus(mIOCP, 0, 0, nullptr))'; New = 'if (!PostQueuedCompletionStatus(nullptr, 0, 0, nullptr))'; Gqcs = 1; Runs = 1; Worker = 2 }
    @{ Name = 'forward_signal'; Old = 'if (bQuit && !PostQueuedCompletionStatus(mIOCP, 0, 0, nullptr))'; New = 'if (bQuit && !PostQueuedCompletionStatus(nullptr, 0, 0, nullptr))'; Gqcs = 1; Runs = 6; Worker = 4 }
)
$base = [IO.File]::ReadAllText("$repo\_lib\NetworkLib\NetworkLib.cpp")
$cfgBase = [IO.File]::ReadAllText("$repo\networkLibProject\server.cfg")
foreach ($c in $cases) {
    if (([regex]::Matches($base, [regex]::Escape($c.Old))).Count -ne 1) { throw "anchor: $($c.Name)" }
    [IO.File]::WriteAllText("$wt\_lib\NetworkLib\NetworkLib.cpp", $base.Replace($c.Old, $c.New), $bom)
    $cfg = $cfgBase -creplace 'gqcsEx = \d+;', "gqcsEx = $($c.Gqcs);" -creplace 'workerCnt = \d+;', "workerCnt = $($c.Worker);"
    [IO.File]::WriteAllText("$wt\networkLibProject\server.cfg", $cfg, $noBom)
    & $msbuild 'networkLibProject\networkLibProject.sln' /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo | Select-String -Pattern 'error ' | ForEach-Object { $_.Line }
    foreach ($r in 1..$c.Runs) {
        $name = "iocpfail_$($c.Name)_r$r"
        $begin = Get-Date
        & "$wt\tools\echo_test.ps1" -Name $name -RunSec 4 -ClientCfgs 'client.cfg' | Out-Null
        $d = Join-Path $root $name
        $srv = @(Get-Content "$d\server.txt" -Encoding Default)
        $ev = Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Application Error';StartTime=$begin} -ErrorAction SilentlyContinue | Where-Object { $_.Message -like '*networkLibProject.exe*' } | Select-Object -First 1
        $code = if ($ev) { (($ev.Message -split "`n")[2].Trim()) } else { '비정상 종료 이벤트 없음' }
        $errs = @(Get-Content "$d\server_err.txt" -Encoding Default)
        "$name 종료완료=$(@($srv | Where-Object { $_ -like '*종료 완료*' }).Count) | $code | stderr $($errs.Count)줄: $($errs | Select-Object -First 1)"
        Get-Process networkLibProject, EchoClient -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
}
"done"
