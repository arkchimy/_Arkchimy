# 게임 서버 시험 전부 - 시험마다 [result] 줄을 모아 마지막에 요약한다
# 먼저 빌드(Game.sln Release)하고, Game\Server\db.cfg(Redis 항목 포함)·Game\Login\login.cfg가 있어야 한다. Redis는 REDIS_DIR(없으면 이 PC 기본 위치)에서 띄운다. DB(db.cfg의 데이터베이스)에 시험 계정(lg·ca·cb·cc·rs·rc·ad_·lr)을 만든다
$tests = 'login', 'chat', 'restart', 'reconnect', 'admin', 'login_race'
$results = @()
foreach ($t in $tests)
{
	"########## $t"
	$out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "$t.ps1") 2>&1
	$out | Where-Object { $_ -match '^\s+\[(OK|NG)\]|^\[result\]|\[새 덤프\]|\[경고\]' }
	$results += @($out | Where-Object { $_ -match '^\[result\]' })
}
"########## 요약"
$results
$fail = @($results | Where-Object { $_ -notmatch ' PASS ' }).Count
if ($fail -eq 0 -and $results.Count -eq $tests.Count) { "[all] PASS ($($tests.Count)개)" } else { "[all] FAIL - 실패·누락 $($tests.Count - @($results | Where-Object { $_ -match ' PASS ' }).Count)개" }
