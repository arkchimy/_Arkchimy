# 채팅방(하나) - 입장한 모두에게 같은 순서로, 보낸 사람 포함. 한글, 길이 경계(1·200 통과, 0·201 끊김), 나간 사람은 빠짐
. "$PSScriptRoot\GameTestCommon.ps1"
$tag = Get-Date -Format 'HHmmss'
$ids = "ca$tag", "cb$tag", "cc$tag"
foreach ($i in $ids) { New-TestCredential $i }

Start-GameServer 'chat'
$a = New-GameClient 'a'; Say $a "login $($ids[0])" 600
$b = New-GameClient 'b'; Say $b "login $($ids[1])" 600
$c = New-GameClient 'c'; Say $c "login $($ids[2])" 1300 # 통계 줄은 1초마다
$roomFull = Get-ServerStat '방인원'
# 두 사람이 동시에 30줄씩 - 섞이는 순서는 매번 다르지만, 받는 세 사람은 같은 순서로 받아야 한다(방 락 안에서 방송)
$a.StandardInput.WriteLine('chatburst 30 A'); $b.StandardInput.WriteLine('chatburst 30 B')
$a.StandardInput.Flush(); $b.StandardInput.Flush()
Start-Sleep -Milliseconds 800
Say $a 'chat 안녕하세요' 400
Say $b 'chatraw 1' 300
Say $b 'chatraw 200' 400
$outC = Close-GameClient $c # 한 사람이 나간 뒤에도 남은 사람끼리 채팅
Start-Sleep -Milliseconds 1300
$roomAfter = Get-ServerStat '방인원'
Say $a 'chat after-leave' 400
$outB = Close-GameClient $b
$outA = Close-GameClient $a
# 길이 경계 - 0바이트·201바이트는 잘못된 패킷으로 끊긴다
$bad0 = New-GameClient 'bad0'; Say $bad0 "login $($ids[0])" 600; Say $bad0 'chatraw 0' 400
$bad0Out = Close-GameClient $bad0
$bad1 = New-GameClient 'bad1'; Say $bad1 "login $($ids[1])" 600; Say $bad1 'chatraw 201' 400
$bad1Out = Close-GameClient $bad1
Stop-GameServer | Out-Null

function Get-Burst($out) { @($out | Where-Object { $_ -match '^\[채팅\] c[ab]\d+: [AB] \d+$' } | ForEach-Object { ($_ -split ': ', 2)[1] }) }
$burstA = Get-Burst $outA; $burstB = Get-Burst $outB; $burstC = Get-Burst $outC
function Test-InOrder($lines, $who) { $nums = @($lines | Where-Object { $_ -like "$who *" } | ForEach-Object { [int]($_ -split ' ')[1] }); ($nums.Count -eq 30) -and (($nums -join ',') -eq ((0..29) -join ',')) }

"===== 판정"
Test-Check '세 사람 입장 → 방 인원 3' ($roomFull -eq 3) "방인원 $roomFull"
Test-Check '동시에 보낸 60줄을 세 사람 모두 받음' ($burstA.Count -eq 60 -and $burstB.Count -eq 60 -and $burstC.Count -eq 60) "A $($burstA.Count), B $($burstB.Count), C $($burstC.Count)"
Test-Check '보낸 사람별 순서가 그대로(A 0–29, B 0–29)' ((Test-InOrder $burstC 'A') -and (Test-InOrder $burstC 'B'))
Test-Check '받는 세 사람의 순서가 같음' ((($burstA -join '|') -eq ($burstB -join '|')) -and (($burstB -join '|') -eq ($burstC -join '|')))
Test-Check '한글 채팅(보낸 사람 포함)' (($outB -match "\[채팅\] $($ids[0]): 안녕하세요") -and ($outA -match "\[채팅\] $($ids[0]): 안녕하세요"))
Test-Check '길이 1·200바이트 통과' (($outA -match "\[채팅\] $($ids[1]): a$") -and ($outA -match "\[채팅\] $($ids[1]): a{200}$"))
Test-Check '나간 사람은 방에서 빠짐(방 인원 2, 남은 사람끼리 채팅)' (($roomAfter -eq 2) -and ($outB -match 'after-leave') -and -not ($outC -match 'after-leave')) "방인원 $roomAfter"
Test-Check '0바이트·201바이트 채팅 → 끊김' (($bad0Out -match '\[연결 끊김\]') -and ($bad1Out -match '\[연결 끊김\]'))
Test-Check '서버: 잘못된패킷 2, 끝난 뒤 세션 0·방 인원 0' ((Get-ServerStat '잘못된패킷') -eq 2 -and (Get-ServerStat '세션') -eq 0 -and (Get-ServerStat '방인원') -eq 0) "잘못된패킷 $(Get-ServerStat '잘못된패킷'), 세션 $(Get-ServerStat '세션'), 방인원 $(Get-ServerStat '방인원')"
Write-Verdict 'chat'
