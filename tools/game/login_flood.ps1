# 로그인 서버 접속 폭주(설계 login_rsa_change.docx 2-7, 테스트 케이스 login_rsa_test_cases.docx R12) - ClientHello만 보내는 접속 50·200·400개를 연 직후
# 정상 로그인 5개가 성공하는지. 서버 임시 RSA 키 쌍은 미리 만들어 두므로(기본 64개) 인증 스레드가 막히지 않고, 떨어진 뒤의 접속은 바로 끊긴다. 약 3분
. "$PSScriptRoot\GameTestCommon.ps1"
$id = "fl$(Get-Date -Format 'HHmmss')"
Start-GameServer 'login_flood'
New-TestCredential $id
$rows = @()
foreach ($n in 0, 50, 200, 400)
{
	$f = $null
	if ($n -gt 0)
	{
		$f = New-GameClient "f$n"
		Say $f "helloflood $n 20" 1500 # 접속을 다 연 뒤
	}
	$c = New-GameClient "c$n"
	Say $c "authflood 5 $id" 14000
	$cOut = Close-GameClient $c
	if ($null -ne $f)
	{
		Start-Sleep 8
		$null = Close-GameClient $f
	}
	$line = "$($cOut | Where-Object { $_ -match '\[동시 로그인\]' } | Select-Object -First 1)"
	$okCnt = if ($line -match '성공=(\d+)') { [int]$Matches[1] } else { -1 }
	$ms = if ($line -match '걸린시간=(\d+)ms') { [int]$Matches[1] } else { -1 }
	$rows += [pscustomobject]@{ Flood = $n; Ok = $okCnt; Ms = $ms }
	Start-Sleep 12 # 기한(10초)으로 남은 접속이 정리될 때까지
}
$stats = @(Get-Content "$GameOut\login.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' })
$maxWorking = ($stats | ForEach-Object { if ($_ -match '처리중워커=(\d+)') { [int]$Matches[1] } } | Measure-Object -Maximum).Maximum
$keyEmpty = Get-LoginStat '키쌍부족끊김'
$loginSessions = Get-LoginStat '세션'
Stop-GameServer

"===== 판정"
foreach ($r in $rows)
{
	Test-Check "ClientHello만 보낸 접속 $($r.Flood)개 직후 정상 로그인 5개 성공" ($r.Ok -eq 5) "성공 $($r.Ok)개, $($r.Ms)ms"
}
Test-Check '정상 로그인 5개가 2초 안(폭주 50개 이상)' (@($rows | Where-Object { $_.Flood -gt 0 -and ($_.Ms -lt 0 -or $_.Ms -ge 2000) }).Count -eq 0) (($rows | ForEach-Object { "$($_.Flood):$($_.Ms)ms" }) -join ', ')
Test-Check '가입·로그인을 처리하는 워커는 워커 수(2) 이하(키 쌍 만들기는 워커가 하지 않음)' ($null -ne $maxWorking -and $maxWorking -le 2) "처리중워커 최대 $maxWorking"
Test-Check '키 쌍이 떨어진 뒤의 접속은 바로 끊김' ($keyEmpty -gt 0) "키쌍부족끊김 $keyEmpty"
Test-Check '로그인 서버 세션 0' ($loginSessions -eq 0)
Write-Verdict 'login_flood'
