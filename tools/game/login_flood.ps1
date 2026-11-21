# 로그인 서버 접속 폭주(설계 login_rsa_change.docx 2-7, 테스트 케이스 login_rsa_test_cases.docx R12) - HelloServer만 보내는 접속 50·200·400개를 연 직후 정상 로그인 5개
# 서버 임시 키 쌍은 늘 loginKeyPool개(기본 256)이고 연결이 끝나야 돌아온다(2026-11-20 - 키 쌍 스레드 없음, 키가 없으면 그 자리에서 만들지 않고 끊는다)
# 키 수 이하의 폭주면 정상 로그인이 성공하고, 키 수를 넘으면 그동안 새 접속은 바로 끊기며 폭주 접속이 요청 기한(10초)으로 정리된 뒤 다시 로그인된다. 약 4분
. "$PSScriptRoot\GameTestCommon.ps1"
$cfgText = [IO.File]::ReadAllText((Join-Path $GameRoot 'Game\Server\server.cfg'))
$keyPool = if ($cfgText -match '(?m)^\s*loginKeyPool\s*=\s*(\d+)') { [int]$Matches[1] } else { 256 }
$id = "fl$(Get-Date -Format 'HHmmss')"
Start-GameServer 'login_flood'
New-TestCredential $id

function Invoke-NormalLogins($tag)
{
	$c = New-GameClient $tag
	Say $c "authflood 5 $id" 14000
	$cOut = Close-GameClient $c
	$line = "$($cOut | Where-Object { $_ -match '\[동시 로그인\]' } | Select-Object -First 1)"
	$okCnt = if ($line -match '성공=(\d+)') { [int]$Matches[1] } else { -1 }
	$ms = if ($line -match '걸린시간=(\d+)ms') { [int]$Matches[1] } else { -1 }
	[pscustomobject]@{ Ok = $okCnt; Ms = $ms }
}

$rows = @()
foreach ($n in 0, 50, 200, 400)
{
	$f = $null
	if ($n -gt 0)
	{
		$f = New-GameClient "f$n"
		Say $f "helloflood $n 20" 1500 # 접속을 다 연 뒤
	}
	$r = Invoke-NormalLogins "c$n"
	if ($null -ne $f)
	{
		Start-Sleep 8
		$null = Close-GameClient $f
	}
	$rows += [pscustomobject]@{ Flood = $n; Ok = $r.Ok; Ms = $r.Ms }
	Start-Sleep 12 # 기한(10초)으로 남은 접속이 정리되고 키가 돌아올 때까지
}
$recover = Invoke-NormalLogins 'recover' # 키 수를 넘는 폭주가 정리된 뒤
$stats = @(Get-Content "$GameOut\login.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' })
$maxWorking = ($stats | ForEach-Object { if ($_ -match '처리중워커=(\d+)') { [int]$Matches[1] } } | Measure-Object -Maximum).Maximum
$keyEmpty = Get-LoginStat '키쌍부족끊김'
$keyReady = Get-LoginStat '키쌍'
$loginSessions = Get-LoginStat '세션'
Stop-GameServer

"===== 판정 (loginKeyPool $keyPool)"
foreach ($r in $rows)
{
	if ($r.Flood -le $keyPool)
	{
		Test-Check "HelloServer만 보낸 접속 $($r.Flood)개(키 수 이하) 직후 정상 로그인 5개가 2초 안에 성공" (($r.Ok -eq 5) -and ($r.Ms -ge 0) -and ($r.Ms -lt 2000)) "성공 $($r.Ok)개, $($r.Ms)ms"
	}
	else
	{
		Test-Check "HelloServer만 보낸 접속 $($r.Flood)개(키 수 초과) 동안 새 접속은 바로 끊김" (($r.Ok -eq 0) -and ($r.Ms -ge 0) -and ($r.Ms -lt 2000)) "성공 $($r.Ok)개, $($r.Ms)ms"
	}
}
Test-Check '키 수를 넘는 폭주가 요청 기한으로 정리된 뒤 정상 로그인 5개 성공' ($recover.Ok -eq 5) "성공 $($recover.Ok)개, $($recover.Ms)ms"
Test-Check '키 쌍이 떨어진 뒤의 접속은 바로 끊김(키쌍부족끊김)' ($keyEmpty -gt 0) "키쌍부족끊김 $keyEmpty"
Test-Check '연결이 모두 끝나면 키가 모두 풀로 돌아옴' ($keyReady -eq $keyPool) "키쌍 $keyReady / $keyPool"
Test-Check '가입·로그인을 처리하는 워커는 워커 수(2) 이하' ($null -ne $maxWorking -and $maxWorking -le 2) "처리중워커 최대 $maxWorking"
Test-Check '로그인 서버 세션 0' ($loginSessions -eq 0)
Write-Verdict 'login_flood'
