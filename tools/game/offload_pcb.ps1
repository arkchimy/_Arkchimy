# 두 PC 인증 분산 측정 - PC B 쪽(게임 서버만). PC A는 offload_pca.ps1(MySQL·Redis·로그인 서버·봇)
# 게임 서버를 띄워 두고 Minutes 동안 1초마다 시각·CPU(코어)·메모리를 game_proc.csv에 한 줄씩 적는다(중간에 Ctrl+C여도 남는다)
# 그동안 PC A가 회차를 여러 번 돌린다. 회차 구간은 PC A의 phases.csv 시각으로 나눈다(두 PC 시계는 w32tm으로 맞춘다)
# -Mode A: 지금 구조(게임 서버는 Redis로 세션 키만 확인) / B: 게임 서버가 인증까지 하는 구조를 흉내(ARK_EMULATE_AUTH=1 -
#   실험폴더\auth_offload_2026_11_21\emulate_auth.patch를 적용해 빌드한 게임 서버여야 한다. 패치가 없는 빌드는 B여도 A와 같다)
# 모드는 게임 서버가 시작할 때 정해진다 - A 회차들을 다 잰 뒤 이 스크립트를 -Mode B로 다시 띄운다
# db.cfg의 host·redisHost는 PC A 사설 IP여야 한다(절차 문서)
# -NoDelay 0|1: server.cfg의 tcpNoDelay를 이 값으로 잠시 바꿔 띄우고 끝나면 되돌린다(TCP_NODELAY 비교, -1이면 server.cfg 그대로)
# 결과: %TEMP%\arkchimy_game_test\offload_pcb_<Mode>[_nd<NoDelay>]\ (server.txt, game_proc.csv)
param(
	[ValidateSet('A', 'B')][string]$Mode = 'A',
	[int]$Minutes = 20,
	[ValidateSet(-1, 0, 1)][int]$NoDelay = -1
)
. "$PSScriptRoot\GameTestCommon.ps1"
if ($Mode -eq 'B') { $env:ARK_EMULATE_AUTH = '1' } else { Remove-Item Env:ARK_EMULATE_AUTH -ErrorAction SilentlyContinue }
$outDir = Join-Path $OutRoot ("offload_pcb_$Mode" + $(if ($NoDelay -ge 0) { "_nd$NoDelay" } else { '' }))
$cfgOrig = if ($NoDelay -ge 0) { Set-ServerCfg 'tcpNoDelay' $NoDelay } else { $null }
New-Item -ItemType Directory -Force $outDir | Out-Null
Remove-Item "$outDir\game_proc.csv" -ErrorAction SilentlyContinue
$script:GameOut = $outDir
$script:GameServerCmd = Start-ConsoleServer $GameExe $outDir 'server'
$watch = [Diagnostics.Stopwatch]::StartNew()
while ($watch.Elapsed.TotalSeconds -lt 30)
{
	if (@(Get-Content "$outDir\server.txt" -Encoding Default -ErrorAction SilentlyContinue) -match '\[stat\]') { break }
	Start-Sleep -Milliseconds 200
}
if ($cfgOrig) { Restore-ServerCfg $cfgOrig } # 게임 서버는 시작할 때만 읽는다 - 바로 되돌려 중간에 멈춰도 server.cfg가 남지 않게
$game = Get-Process GameServer -ErrorAction SilentlyContinue
if (-not $game)
{
	"[pcb] 게임 서버가 뜨지 않음 - server_err.txt(PC A의 MySQL에 닿는지, db.cfg host)"
	Get-Content "$outDir\server_err.txt" -Encoding Default
	return
}
"[pcb] 게임 서버 준비 $(Get-Date -Format 'HH:mm:ss') 모드 $Mode tcpNoDelay $NoDelay - ${Minutes}분 동안 기록. PC A에서 회차를 돌린다"
Get-Content "$outDir\server.txt" -Encoding Default | Where-Object { $_ -match '측정용' }

$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText("$outDir\game_proc.csv", "Time,GameCpu,GamePrivMB`n", $utf8)
$t0 = Get-Date
$prev = $game.TotalProcessorTime.TotalSeconds
for ($s = 1; $s -le $Minutes * 60; $s++)
{
	$wait = ($t0.AddSeconds($s) - (Get-Date)).TotalMilliseconds
	if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
	$game.Refresh()
	if ($game.HasExited) { "[pcb] 게임 서버가 끝남"; break }
	$now = $game.TotalProcessorTime.TotalSeconds
	[IO.File]::AppendAllText("$outDir\game_proc.csv", ("{0},{1},{2}`n" -f (Get-Date -Format 'HH:mm:ss'), [math]::Round($now - $prev, 3), [math]::Round($game.PrivateMemorySize64 / 1MB, 1)), $utf8)
	$prev = $now
}
Send-QuitKey $GameServerCmd 'GameServer.exe' 'game' | Out-Null
Start-Sleep 3
"===== PC B 모드 $Mode 끝 $(Get-Date -Format 'HH:mm:ss') - $outDir\game_proc.csv"
Get-Content "$outDir\server.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
Get-Content "$outDir\server.txt" -Encoding Default | Where-Object { $_ -match '^\[main\]' } | Select-Object -Last 2
