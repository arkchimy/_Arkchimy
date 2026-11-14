# 장시간 시험 - 봇 여럿이 던전·경매·장비·접속 끊기를 반복하는 동안 서버 통계를 1분마다 모으고, 끝나면 판정한다
# 사용: .\tools\game\soak.ps1 -Minutes 60 -Bots 30
# 판정: 봇의 예상밖끊김·프로토콜오류 0, 서버 DB실패·잘못된패킷·넘침끊김·송신정체끊김 0, 봇이 끝난 뒤 세션 0, 정상 종료, 새 덤프 없음,
#       봇 계정의 DB 골드 = 로그 합계, 닫힌 경매를 가리키는 아이템 없음. 메모리·핸들은 예열 뒤와 마지막을 비교해 보여 준다(기준 이상이면 FAIL)
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

$statKeys = '세션', '로그인', '계정', '파티', '경매', 'DB대기', 'DB실패', '잘못된패킷', '넘침끊김', '송신정체끊김'
function Read-Sample
{
	$line = Get-Content "$out\server.txt" -Encoding Default -Tail 3 | Where-Object { $_ -match '\[stat\]' } | Select-Object -Last 1
	if (-not $line) { return $null }
	$row = [ordered]@{ Time = (Get-Date -Format 'HH:mm:ss') }
	$row['MemMB'] = if ($line -match '메모리=(\d+)MB') { [int]$Matches[1] } else { -1 }
	$row['Handles'] = if ($line -match '핸들=(\d+)') { [int]$Matches[1] } else { -1 }
	foreach ($k in $statKeys) { $row[$k] = if ($line -match "$k=(\d+)") { [int64]$Matches[1] } else { -1 } }
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
		[void]$samples.Add($s)
		$s | Export-Csv "$out\samples.csv" -NoTypeInformation -Append -Encoding UTF8
		$botLine = Get-Content "$out\bot.txt" -Encoding Default -Tail 1
		"[soak] ${minute}분 메모리 $($s.MemMB)MB 핸들 $($s.Handles) 세션 $($s.'세션') 로그인 $($s.'로그인') 파티 $($s.'파티') 경매 $($s.'경매') DB대기 $($s.'DB대기') DB실패 $($s.'DB실패') | $(($botLine -replace '^\[bot\] ', '').Substring(0, [Math]::Min(120, ($botLine -replace '^\[bot\] ', '').Length)))"
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
$dangling = "$(Invoke-GameSql "SELECT COUNT(*) FROM items i LEFT JOIN auctions a ON a.auction_id = i.auction_id WHERE i.auction_id IS NOT NULL AND (a.auction_id IS NULL OR a.state <> 'open')")".Trim()

"===== 봇"
$botResult
"===== 판정"
Test-Check '봇: 예상밖끊김 0, 프로토콜오류 0' ($botResult -match 'PASS') "$($botResult | Select-Object -First 1)"
Test-Check '서버: DB실패 0' ($final -and $final.'DB실패' -eq 0)
Test-Check '서버: 잘못된패킷·넘침끊김·송신정체끊김 0' ($final -and $final.'잘못된패킷' -eq 0 -and $final.'넘침끊김' -eq 0 -and $final.'송신정체끊김' -eq 0)
Test-Check '봇이 끝난 뒤 세션 0' ($final -and $final.'세션' -eq 0) "세션 $($final.'세션')"
Test-Check '서버 정상 종료' ($serverLines -match '종료 완료')
Test-Check '새 덤프 없음' (-not (Get-ChildItem $GameBin -Filter *.dmp | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-($Minutes + 5)) }))
Test-Check "봇 계정의 DB 골드 = 로그 합계" ((Test-GoldMatchesLog "$Prefix%") -eq 0)
Test-Check '닫힌 경매를 가리키는 아이템 없음' ($dangling -eq '0') "${dangling}개"
Test-Check "메모리 증가 ${MaxGrowthPercent}% 이하(예열 뒤 → 마지막)" ($growth -le $MaxGrowthPercent) "$($warm.MemMB)MB → $($last.MemMB)MB ($growth%), 핸들 $($warm.Handles) → $($last.Handles) ($handleGrowth)"
Write-Verdict "soak ${Minutes}분 봇 ${Bots}개"
