param(
    [string]$Name = "echo",
    [int]$RunSec = 20,
    [string[]]$ClientCfgs = @("client.cfg"),	# 클라 cfg - 여러 개면 동시에 띄운다. "폴더/cfg"면 그 폴더의 클라(예: LoadClient/client.cfg), 이름만 쓰면 EchoClient
    [string]$OutRoot = (Join-Path $env:TEMP "arkchimy_echo_test"),
    [string]$ServerExe = ""	# 비우면 이 저장소의 Release 서버. 수정 전후 비교 때 다른 빌드의 서버를 지정한다 (설정은 항상 이 저장소의 server.cfg)
)

# 서버 → 클라(들) → RunSec초 → 클라 종료 → 세션 0 확인 → 서버에 Ctrl+C → 정상 종료 확인
# 실행 전에 networkLibProject.sln, EchoClient.sln을 Release x64로 빌드해 둘 것
# 예: .\tools\echo_test.ps1 -Name stepE -RunSec 15 -ClientCfgs client.cfg,client_badPacket.cfg
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tools = Join-Path $root "tools"
$out = Join-Path $OutRoot $Name
New-Item -ItemType Directory -Force $out | Out-Null

if (-not $ServerExe) { $ServerExe = "$root\networkLibProject\x64\Release\networkLibProject.exe" }
$srv = Start-Process -FilePath $ServerExe `
    -WorkingDirectory "$root\networkLibProject" `
    -RedirectStandardOutput "$out\server.txt" -RedirectStandardError "$out\server_err.txt" -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2

$clis = @()
$i = 0
foreach ($spec in $ClientCfgs) {
    $proj = "EchoClient"
    $cfg = $spec
    if ($spec.Contains("/")) {
        $proj, $cfg = $spec.Split("/", 2)
    }
    # 클라는 작업 폴더의 client.cfg를 읽으므로 클라마다 폴더를 따로 만든다
    $dir = Join-Path $out "client$i"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item (Join-Path "$root\$proj" $cfg) (Join-Path $dir "client.cfg") -Force
    $clis += Start-Process -FilePath "$root\$proj\x64\Release\$proj.exe" -WorkingDirectory $dir `
        -RedirectStandardOutput "$out\client$i.txt" -RedirectStandardError "$out\client${i}_err.txt" -WindowStyle Hidden -PassThru
    $i++
}

Start-Sleep -Seconds $RunSec
foreach ($c in $clis) { if (-not $c.HasExited) { Stop-Process -Id $c.Id -Force } }	# EchoClient는 Q 키로만 끝나서 강제 종료
Start-Sleep -Seconds 3

Start-Process powershell -ArgumentList "-NoProfile","-ExecutionPolicy","Bypass","-File","`"$tools\send_ctrlc.ps1`"","-TargetPid",$srv.Id,"-LogPath","`"$out\ctrlc.txt`"" -WindowStyle Hidden -Wait | Out-Null
$exited = $srv.WaitForExit(15000)
if (-not $exited) { Stop-Process -Id $srv.Id -Force }

"=== 결과 폴더: $out ==="
"=== server exited by Ctrl+C: $exited ==="
"--- server ---"
Get-Content "$out\server.txt" -Encoding Default
Get-Content "$out\server_err.txt" -Encoding Default
for ($j = 0; $j -lt $clis.Count; $j++) {
    "--- client$j ($($ClientCfgs[$j])) ---"
    Get-Content "$out\client$j.txt" -Encoding Default
    Get-Content "$out\client${j}_err.txt" -Encoding Default
}
