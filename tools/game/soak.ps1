# 장시간 시험 - 봇 여럿이 입장·채팅·접속 끊기를 반복하는 동안 서버 통계를 1분마다 모으고, 끝나면 판정한다
# 사용: .\tools\game\soak.ps1 -Minutes 60 -Bots 30
# 판정: 봇의 예상밖끊김·프로토콜오류 0, 서버 DB실패·잘못된패킷·넘침끊김·송신정체끊김·입장확인실패 0, 봇이 끝난 뒤 세션 0, 정상 종료, 새 덤프 없음,
#       봇 계정 행 = 봇 수. 메모리·핸들은 예열 뒤와 마지막을 비교해 보여 준다(기준 이상이면 FAIL)
# 결과: %TEMP%\arkchimy_game_test\soak\ (server.txt, bot.txt, samples.csv)
param(
	[int]$Minutes = 60,
	[int]$Bots = 30,
	[string]$Prefix = 'soak',
	[double]$MaxGrowthPercent = 20 # 예열 뒤 → 마지막 메모리 증가가 이보다 크면 FAIL
)
. "$PSScriptRoot\GameTestCommon.ps1"
$botExe = Join-Path $GameBin 'GameBot.exe'
Start-GameServer 'soak'
$out = $GameOut
Remove-Item "$out\samples.csv", "$out\bot.txt", "$out\bot_err.txt" -ErrorAction SilentlyContinue
$botProc = Start-Process cmd.exe -ArgumentList "/s /c `"`"$botExe`" $Bots $($Minutes * 60) $Prefix > `"$out\bot.txt`" 2> `"$out\bot_err.txt`"`"" -WorkingDirectory $GameBin -WindowStyle Hidden -PassThru
"[soak] 시작 $(Get-Date -Format 'HH:mm:ss') - 봇 ${Bots}개, ${Minutes}분. 결과 폴더 $out"

$statKeys = '세션', '로그인', '계정', '방인원', '채팅', 'DB대기', 'DB실패', '잘못된패킷', '넘침끊김', '송신정체끊김', '입장확인실패', '입장거절'
$loginKeys = '로그인성공', '로그인실패', '처리중워커'
function Read-Sample
{
	$line = Get-Content "$out\server.txt" -Encoding Default -Tail 3 | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
	if (-not $line) { return $null }
	$row = [ordered]@{ Time = (Get-Date -Format 'HH:mm:ss') }
	$row['MemMB'] = if ($line -match '메모리=(\d+)MB') { [int]$Matches[1] } else { -1 }
	$row['Handles'] = if ($line -match '핸들=(\d+)') { [int]$Matches[1] } else { -1 }
	foreach ($k in $statKeys) { $row[$k] = if ($line -match "$k=(\d+)") { [int64]$Matches[1] } else { -1 } }
	# 로그인 서버 - 메모리·핸들과 누적 로그인 수, Redis에 남은 세션 키 수(30초 뒤 사라지므로 봇 수 근처에서 늘지 않아야 한다)
	$login = Get-Content "$out\login.txt" -Encoding Default -Tail 3 | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
	$row['LoginMemMB'] = if ($login -match '메모리=(\d+)MB') { [int]$Matches[1] } else { -1 }
	$row['LoginHandles'] = if ($login -match '핸들=(\d+)') { [int]$Matches[1] } else { -1 }
	$row['LoginSessions'] = if ($login -match '세션=(\d+)') { [int64]$Matches[1] } else { -1 }
	foreach ($k in $loginKeys) { $row[$k] = if ($login -match "$k=(\d+)") { [int64]$Matches[1] } else { -1 } }
	$redisKeys = "$(Invoke-Redis @('DBSIZE'))".Trim()
	$row['RedisKeys'] = if ($redisKeys -match '^\d+$') { [int64]$redisKeys } else { -1 }
	[pscustomobject]$row
}

$samples = New-Object System.Collections.ArrayList
$minute = 0
while (-not $botProc.HasExited)
{
	Start-Sleep 60
	++$minute
	$s = Read-Sample
	if ($s)
	{
		$botLine = Get-Content "$out\bot.txt" -Encoding Default -Tail 1
		$s | Add-Member -NotePropertyName BotActions -NotePropertyValue $(if ($botLine -match '행동=(\d+)') { [int64]$Matches[1] } else { -1 })
		[void]$samples.Add($s)
		$s | Export-Csv "$out\samples.csv" -NoTypeInformation -Append -Encoding UTF8
		"[soak] ${minute}분 메모리 $($s.MemMB)MB 핸들 $($s.Handles) 세션 $($s.'세션') 로그인 $($s.'로그인') 방인원 $($s.'방인원') 채팅 $($s.'채팅') DB대기 $($s.'DB대기') DB실패 $($s.'DB실패') 로그인서버 $($s.LoginMemMB)MB 키 $($s.RedisKeys) | $(($botLine -replace '^\[bot\] ', '').Substring(0, [Math]::Min(120, ($botLine -replace '^\[bot\] ', '').Length)))"
	}
}
Start-Sleep 5 # 봇이 끊은 세션이 모두 해제되기를
$final = Read-Sample
Stop-GameServer | Out-Null

$botResult = Get-Content "$out\bot.txt" -Encoding Default | Where-Object { $_ -match '^\[bot-result\]' }
$serverLines = Get-Content "$out\server.txt" -Encoding Default
$warm = if ($samples.Count -ge 5) { $samples[[int]($samples.Count * 0.2)] } elseif ($samples.Count -gt 0) { $samples[0] } else { $null }
$last = if ($samples.Count -gt 0) { $samples[$samples.Count - 1] } else { $null }
$growth = if ($warm -and $last -and $warm.MemMB -gt 0) { [math]::Round(($last.MemMB - $warm.MemMB) * 100.0 / $warm.MemMB, 1) } else { 0 }
$handleGrowth = if ($warm -and $last) { $last.Handles - $warm.Handles } else { 0 }
# 멈춤: 봇이 도는 동안(마지막 1분 제외) 매 분 봇의 행동 수가 늘었는가 - 서버가 멈춰도 봇은 끊기지 않으므로 따로 본다
$stalls = 0
for ($i = 1; $i -lt $samples.Count - 1; ++$i) { if ($samples[$i].BotActions -le $samples[$i - 1].BotActions) { ++$stalls } }
$maxPending = ($samples | Measure-Object -Property 'DB대기' -Maximum).Maximum
$maxRedisKeys = ($samples | Measure-Object -Property 'RedisKeys' -Maximum).Maximum
$loginGrowth = if ($warm -and $last -and $warm.LoginMemMB -gt 0) { [math]::Round(($last.LoginMemMB - $warm.LoginMemMB) * 100.0 / $warm.LoginMemMB, 1) } else { 0 }
$loginLines = Get-Content "$out\login.txt" -Encoding Default
$botRows = "$(Invoke-GameSql "SELECT COUNT(*) FROM accounts WHERE CAST(account_id AS CHAR) LIKE '$Prefix%'")".Trim()

"===== 봇"
$botResult
"===== 판정"
Test-Check '봇: 예상밖끊김 0, 프로토콜오류 0, 인증실패 0' ($botResult -match 'PASS') "$($botResult | Select-Object -First 1)"
Test-Check '서버: DB실패 0' ($final -and $final.'DB실패' -eq 0)
# 게임 서버는 입장 실패면 응답 없이 끊어 봇은 이유를 모른다 - 키가 없거나 틀린 입장(진짜 실패)은 서버 통계로 본다. 입장거절(이미 접속 중 - 스스로 끊은 직후)은 정상
Test-Check '서버: 입장확인실패 0(받은 세션 키로 모두 입장)' ($final -and $final.'입장확인실패' -eq 0) "입장확인실패 $($final.'입장확인실패'), 입장거절 $($final.'입장거절')"
Test-Check '서버: 잘못된패킷·넘침끊김·송신정체끊김 0' ($final -and $final.'잘못된패킷' -eq 0 -and $final.'넘침끊김' -eq 0 -and $final.'송신정체끊김' -eq 0)
Test-Check '봇이 끝난 뒤 세션 0·방 인원 0' ($final -and $final.'세션' -eq 0 -and $final.'방인원' -eq 0) "세션 $($final.'세션'), 방인원 $($final.'방인원')"
Test-Check '서버 정상 종료' ($serverLines -match '종료 완료')
Test-Check '새 덤프 없음' (-not (Get-ChildItem $GameBin -Filter *.dmp | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-($Minutes + 5)) }))
Test-Check '봇 계정 행 = 봇 수(같은 아이디로 여러 번 입장해도 행은 하나)' ($botRows -eq "$Bots") "${botRows}줄"
Test-Check '멈춤 없음(매 분 봇 행동 수가 늚)' ($stalls -eq 0) "멈춘 분 $stalls"
Test-Check 'DB 대기열이 쌓이지 않음(최대 100 미만)' ($maxPending -lt 100) "최대 $maxPending"
Test-Check "메모리 증가 ${MaxGrowthPercent}% 이하(예열 뒤 → 마지막)" ($growth -le $MaxGrowthPercent) "$($warm.MemMB)MB → $($last.MemMB)MB ($growth%), 핸들 $($warm.Handles) → $($last.Handles) ($handleGrowth)"
Test-Check "로그인 서버: 메모리 증가 ${MaxGrowthPercent}% 이하, 잘못된패킷 0, 정상 종료" (($loginGrowth -le $MaxGrowthPercent) -and (Get-LoginStat '잘못된패킷') -eq 0 -and ($loginLines -match '종료 완료')) "$($warm.LoginMemMB)MB → $($last.LoginMemMB)MB ($loginGrowth%), 핸들 $($warm.LoginHandles) → $($last.LoginHandles)"
Test-Check "Redis 세션 키가 쌓이지 않음(최대 봇 수 이하)" ($maxRedisKeys -le $Bots) "최대 $maxRedisKeys"
Write-Verdict "soak ${Minutes}분 봇 ${Bots}개"
