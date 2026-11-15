# 로그인 서버와 게임 서버 입장 확인(설계 login_server.docx 9-2) - 가입·중복 가입·틀린 비밀번호·한 세션 5번 실패 끊기·암호문 변조·가짜 서버 키·
# 세션 키 재사용 거절·가짜 첫 메시지로 키가 지워지지 않음·30초 만료·Redis에 키가 남지 않음
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss' # 매번 처음 보는 아이디
$id = "lg$tag"
Start-GameServer 'login'

# 1. 가입·로그인 서버 결과
$u = New-GameClient 'u'
Say $u "register $id" 400
Say $u "register $id" 400
Say $u "register ${id}b 1234" 200
Say $u "login $id wrong-password" 400
Say $u "login nouser$tag" 400
Say $u "login $id" 600
$uOut = Close-GameClient $u

# 2. 암호 통로 - 한 연결에서 5번 틀리면 끊김, 변조한 암호문은 응답 없이 끊김, 서버 공개 키가 다르면 클라가 비밀번호를 보내지 않음
$r = New-GameClient 'r'
Say $r "authrepeat $id wrong-password 6" 1500
Say $r "authbad $id" 600
Say $r "pinkey $('00' * 32)" 100
Say $r "login $id" 600
$rOut = Close-GameClient $r
$repeat = if ("$rOut" -match '\[반복 로그인\] 응답 (\d+)번') { [int]$Matches[1] } else { -1 }
# 3. 세션 키 재사용·가짜 첫 메시지
$a = New-GameClient 'a'; Say $a "auth $id" 500
$aOut = Close-GameClient $a
$keyLine = $aOut | Where-Object { $_ -match '\[세션 키\] no=(\d+) key=([0-9a-f]{64})' } | Select-Object -First 1
$no = $Matches[1]; $key = $Matches[2]
$fake = New-GameClient 'fake'; Say $fake "enter $no $key bad" 500
$fakeOut = Close-GameClient $fake
$real = New-GameClient 'real'; Say $real "enter $no $key" 600
$realOut = Close-GameClient $real
$again = New-GameClient 'again'; Say $again "enter $no $key" 500
$againOut = Close-GameClient $again

# 4. 30초 만료
$e = New-GameClient 'e'; Say $e "auth $id" 500
$eOut = Close-GameClient $e
$null = $eOut | Where-Object { $_ -match '\[세션 키\] no=(\d+) key=([0-9a-f]{64})' } | Select-Object -First 1
$eNo = $Matches[1]; $eKey = $Matches[2]
Start-Sleep 31
$late = New-GameClient 'late'; Say $late "enter $eNo $eKey" 500
$lateOut = Close-GameClient $late
Start-Sleep 2
# 5. 게임 구간 암호화 - 입장 뒤 인벤토리가 오는지(암호문으로 주고받음), 변조한 암호문은 끊김, 입장 전의 평문 명령은 끊김
$g = New-GameClient 'g'; Say $g "login $id" 600; Say $g 'inv' 400; Say $g 'tamper' 400
$gOut = Close-GameClient $g
$p = New-GameClient 'p'; Say $p 'inv' 400
$pOut = Close-GameClient $p
$redisKeys = "$(Invoke-Redis @('DBSIZE'))".Trim()

# 5. Redis가 꺼지면 로그인은 오래 기다리지 않고 실패한다(연결 1초 제한, 실패 뒤 5초는 바로 실패) - 인증·DB 스레드가 붙잡히지 않게
Stop-Redis
$watch = [Diagnostics.Stopwatch]::StartNew()
$down = New-GameClient 'down'
Say $down "login $id" 100
Say $down "login $id" 100
Say $down "login $id" 100
$downOut = Close-GameClient $down
$watch.Stop()
Stop-GameServer

"===== 판정"
Test-Check '가입 성공' ($uOut -match '\[가입\] 가입했습니다')
Test-Check '같은 아이디 다시 가입 → 이미 있음' ($uOut -match '\[가입\] 이미 있는 아이디')
Test-Check '짧은 비밀번호 → 거절' ($uOut -match '\[가입\] 비밀번호는 8~64바이트')
Test-Check '틀린 비밀번호·없는 아이디 → 같은 실패' (@($uOut -match '\[로그인 서버\] 아이디 또는 비밀번호가 틀립니다').Count -eq 2)
Test-Check '맞는 비밀번호 → 게임 서버 입장' ($uOut -match '\[로그인\] 성공')
Test-Check '한 연결 5번 실패 뒤 끊김(6번째는 응답 없음)' ($repeat -eq 5 -or $repeat -eq 4) "응답 ${repeat}번(5번째 응답은 끊기와 겹쳐 못 받을 수 있다)"
Test-Check '변조한 암호문 → 응답 없이 끊김' ($rOut -match '\[변조\] 서버가 응답 없이 끊음')
Test-Check '서버 공개 키가 다르면 → 서버 확인 실패, 비밀번호를 보내지 않음' ($rOut -match '\[로그인 서버\] 서버 확인 실패')
Test-Check '세션 키 받음' ($null -ne $keyLine)
Test-Check '틀린 증명 → 입장 실패' ($fakeOut -match '\[로그인\] 입장 확인 실패')
Test-Check '틀린 증명 뒤에도 진짜 키로 입장(키가 지워지지 않음)' ($realOut -match '\[로그인\] 성공')
Test-Check '같은 키 다시 → 입장 실패(한 번만)' ($againOut -match '\[로그인\] 입장 확인 실패')
Test-Check '30초 지난 키 → 입장 실패' ($lateOut -match '\[로그인\] 입장 확인 실패')
Test-Check 'Redis에 남은 키 0' ($redisKeys -eq '0') "DBSIZE $redisKeys"
Test-Check '입장 뒤 암호문으로 인벤토리를 주고받음' (($gOut -match '\[로그인\] 성공') -and ($gOut -match '골드'))
Test-Check '게임 구간 변조한 암호문 → 끊김' ($gOut -match '\[연결 끊김\]')
Test-Check '입장 전 평문 명령 → 끊김' ($pOut -match '\[연결 끊김\]')
Test-Check 'Redis가 꺼지면 로그인 3번 모두 서버 오류' (@($downOut -match '\[로그인 서버\] 서버 오류').Count -eq 3)
Test-Check 'Redis가 꺼져도 오래 기다리지 않음(3번에 5초 안)' ($watch.Elapsed.TotalSeconds -lt 5) "$([math]::Round($watch.Elapsed.TotalSeconds, 1))초"
Test-Check '게임 서버 세션 0' ((Get-ServerStat '세션') -eq 0)
Test-Check '로그인 서버 세션 0' ((Get-LoginStat '세션') -eq 0)
Write-Verdict 'login'
