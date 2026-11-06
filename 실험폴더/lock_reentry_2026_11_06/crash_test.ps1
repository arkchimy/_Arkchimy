$repo = 'C:\_ArkchimyProject'
$wt = 'C:\_Arkchimy_deadlock'
$root = Join-Path $env:TEMP 'arkchimy_echo_test'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
Copy-Item "$repo\_lib\NetworkLib\NetworkLib.cpp" "$wt\_lib\NetworkLib\NetworkLib.cpp" -Force
Copy-Item "$repo\_lib\NetworkLib\NetworkLib.h" "$wt\_lib\NetworkLib\NetworkLib.h" -Force
$bom = New-Object Text.UTF8Encoding($true)
$p = "$wt\_lib\NetworkLib\NetworkLib.cpp"
$t = [IO.File]::ReadAllText($p)
$old = 'if (!PostQueuedCompletionStatus(mIOCP, 0, reinterpret_cast<ULONG_PTR>(&session), &session.mReleaseOv))'
if (([regex]::Matches($t, [regex]::Escape($old))).Count -ne 1) { throw 'anchor' }
$t = $t.Replace($old, 'if (!PostQueuedCompletionStatus(nullptr, 0, reinterpret_cast<ULONG_PTR>(&session), &session.mReleaseOv)) // [test] real failure')
[IO.File]::WriteAllText($p, $t, $bom)
$p = "$wt\networkLibProject\server.cfg"
$t = [IO.File]::ReadAllText($p)
[IO.File]::WriteAllText($p, ($t -creplace 'chatRoomCnt = 0;', 'chatRoomCnt = 16;'), (New-Object Text.UTF8Encoding($false)))
Set-Location $wt
foreach ($sln in 'networkLibProject\networkLibProject.sln', 'LoadClient\LoadClient.sln') {
    & $msbuild $sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo | Select-String -Pattern 'error |warning C' | ForEach-Object { $_.Line }
}
foreach ($w in 2, 4) {
    $c = "$wt\networkLibProject\server.cfg"
    $t = [IO.File]::ReadAllText($c)
    [IO.File]::WriteAllText($c, ($t -creplace 'workerCnt = \d+;', "workerCnt = $w;"), (New-Object Text.UTF8Encoding($false)))
    foreach ($r in 1..2) {
        $name = "crash_w${w}_r$r"
        $begin = Get-Date
        & "$wt\tools\echo_test.ps1" -Name $name -RunSec 10 -ClientCfgs 'LoadClient/client_chat.cfg','LoadClient/client_chatNoRecv.cfg' | Out-Null
        $d = Join-Path $root $name
        $srv = @(Get-Content "$d\server.txt" -Encoding Default)
        $ev = Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Application Error';StartTime=$begin} -ErrorAction SilentlyContinue | Where-Object { $_.Message -like '*networkLibProject.exe*' } | Select-Object -First 1
        $code = if ($ev) { (($ev.Message -split "`n")[2].Trim()) + ' / ' + (($ev.Message -split "`n")[1].Trim() -replace ', version.*', '') } else { '이벤트 없음' }
        "$name stat=$(@($srv | Where-Object { $_ -like '`[stat`]*' }).Count) 종료완료=$(@($srv | Where-Object { $_ -like '*종료 완료*' }).Count) | $code"
        "  stderr: " + ((Get-Content "$d\server_err.txt" -Encoding Default) -join ' / ')
        Get-Process networkLibProject, LoadClient -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
}
"done"
