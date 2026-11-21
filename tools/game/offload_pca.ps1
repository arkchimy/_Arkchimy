# 두 PC 인증 분산 측정 - PC A 쪽(MySQL·Redis·로그인 서버·봇). PC B는 offload_pcb.ps1(게임 서버만)
# 한 회: 채팅 봇 Steady개 시작 → WarmSec 뒤 입장 폭주 봇 Churn개 BurstSec → AfterSec 회복
# PC B의 게임 서버(offload_pcb.ps1)를 먼저 켜 둔다. 단계별 시작 시각을 phases.csv에 적고, PC B game_proc.csv와 시각으로 맞춘다(시계는 w32tm으로)
# -Prep: 시각을 기다리지 않고 폭주 봇 계정을 가입시키고 게임 계정 행을 만든다(측정에 가입 해시·첫 입장 INSERT가 섞이지 않게, 한 번만)
# 결과: %TEMP%\arkchimy_game_test\offload_<Tag>\ (login.txt, steady.txt, churn.txt, login_proc.csv)
param(
	[Parameter(Mandatory = $true)][string]$GameIP,   # PC B 사설 IP
	[Parameter(Mandatory = $true)][string]$BindIP,   # PC A 사설 IP - Redis가 이 주소로도 받는다(PC B 게임 서버용)
	[string]$StartAt = '',                           # 'HH:mm:ss' - 비우면 로그인 서버가 준비되자마자 시작
	[string]$Tag = 'run',
	[int]$Steady = 30,
	[int]$Churn = 100,
	[int]$WarmSec = 60,
	[int]$BurstSec = 60,
	[int]$AfterSec = 60,
	[switch]$Prep
)
. "$PSScriptRoot\GameTestCommon.ps1"
$botExe = Join-Path $GameBin 'GameBot.exe'
$outDir = Join-Path $OutRoot "offload_$Tag"
New-Item -ItemType Directory -Force $outDir | Out-Null
$script:GameOut = $outDir

# Redis - 127.0.0.1과 PC A 사설 IP 둘 다에서 받는다(비밀번호는 db.cfg, 저장 끔). 방화벽은 PC B IP에서 오는 6379만 연다(절차 문서)
$kv = Read-DbCfg
$script:RedisProc = $null
if (-not (Get-NetTCPConnection -LocalPort ([int]$kv['redisPort']) -State Listen -ErrorAction SilentlyContinue))
{
	$conf = Join-Path $OutRoot 'redis_offload.conf'
	[IO.File]::WriteAllText($conf, "port $($kv['redisPort'])`nbind 127.0.0.1 $BindIP`nrequirepass $($kv['redisPassword'])`nsave `"`"`nappendonly no`n", (New-Object Text.UTF8Encoding($false)))
	$script:RedisProc = Start-Process (Join-Path $RedisDir 'redis-server.exe') -ArgumentList "`"$conf`"" -WorkingDirectory $OutRoot -WindowStyle Hidden -PassThru
	Start-Sleep 1
	Remove-Item $conf -ErrorAction SilentlyContinue
}
$script:LoginServerCmd = Start-ConsoleServer $LoginExe $outDir 'login'
$watch = [Diagnostics.Stopwatch]::StartNew()
while ($watch.Elapsed.TotalSeconds -lt 60)
{
	$lines = @(Get-Content "$outDir\login.txt" -Encoding Default -ErrorAction SilentlyContinue)
	if (($lines -match '\[main\] 로그인 서버 포트') -and ($lines -match '\[stat\]')) { break }
	Start-Sleep -Milliseconds 200
}
"[pca] 로그인 서버 준비 $(Get-Date -Format 'HH:mm:ss')"

function Start-Bot($cnt, $sec, $prefix, $mode, $file)
{
	return Start-Process cmd.exe -ArgumentList "/s /c `"`"$botExe`" $cnt $sec $prefix $GameIP 33000 33002 $mode 127.0.0.1 > `"$outDir\$file.txt`" 2> `"$outDir\${file}_err.txt`"`"" -WorkingDirectory $GameBin -WindowStyle Hidden -PassThru
}

function Stop-All
{
	Send-QuitKey $LoginServerCmd 'LoginServer.exe' 'login' | Out-Null
	Start-Sleep 3
	Stop-Redis
}

if ($Prep)
{
	$p = Start-Bot $Churn 40 'churn' 'churn' 'churn'
	$p.WaitForExit()
	Get-Content "$outDir\churn.txt" -Encoding Default | Select-Object -Last 2
	Stop-All
	return
}

if ($StartAt -ne '')
{
	$start = [datetime]::ParseExact($StartAt, 'HH:mm:ss', $null)
	if ($start -lt (Get-Date)) { throw "StartAt($StartAt)이 이미 지났다" }
	"[pca] $StartAt까지 기다림"
	while ((Get-Date) -lt $start) { Start-Sleep -Milliseconds 100 }
}
else
{
	$start = (Get-Date).AddSeconds(1)
	while ((Get-Date) -lt $start) { Start-Sleep -Milliseconds 50 }
}
$warmEnd = $start.AddSeconds($WarmSec); $burstEnd = $warmEnd.AddSeconds($BurstSec)
@("Tag,Phase,From,To", "$Tag,warm,$($start.ToString('HH:mm:ss')),$($warmEnd.ToString('HH:mm:ss'))", "$Tag,burst,$($warmEnd.ToString('HH:mm:ss')),$($burstEnd.ToString('HH:mm:ss'))",
	"$Tag,after,$($burstEnd.ToString('HH:mm:ss')),$($burstEnd.AddSeconds($AfterSec).ToString('HH:mm:ss'))") | Set-Content "$outDir\phases.csv" -Encoding UTF8
"[pca] 시작 $($start.ToString('HH:mm:ss')) - 폭주 $($warmEnd.ToString('HH:mm:ss'))–$($burstEnd.ToString('HH:mm:ss'))"
$total = $WarmSec + $BurstSec + $AfterSec
$steadyProc = Start-Bot $Steady ($total + 5) 'soak' 'chat' 'steady'
$login = Get-Process LoginServer
$prev = $login.TotalProcessorTime.TotalSeconds
$rows = New-Object System.Collections.ArrayList
for ($s = 1; $s -le $total; $s++)
{
	if ($s -eq $WarmSec + 1) { $churnProc = Start-Bot $Churn $BurstSec 'churn' 'churn' 'churn' }
	$wait = ($start.AddSeconds($s) - (Get-Date)).TotalMilliseconds
	if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
	$login.Refresh()
	$now = $login.TotalProcessorTime.TotalSeconds
	$phase = if ($s -le $WarmSec) { 'warm' } elseif ($s -le $WarmSec + $BurstSec) { 'burst' } else { 'after' }
	[void]$rows.Add([pscustomobject]@{ Sec = $s; Phase = $phase; LoginCpu = [math]::Round($now - $prev, 3); LoginPrivMB = [math]::Round($login.PrivateMemorySize64 / 1MB, 1) })
	$prev = $now
}
$rows | Export-Csv "$outDir\login_proc.csv" -NoTypeInformation -Encoding UTF8
$steadyProc.WaitForExit(30000) | Out-Null
Stop-All
"===== $Tag - 채팅 왕복(채팅 봇, 5초 구간; 폭주는 t=$WarmSec 부터 $($WarmSec + $BurstSec)초)"
Get-Content "$outDir\steady.txt" -Encoding Default | Where-Object { $_ -match '^\[lat\]|bot-result' }
"--- 폭주 봇"
Get-Content "$outDir\churn.txt" -Encoding Default | Where-Object { $_ -match '^\[lat\]|bot-result' }
