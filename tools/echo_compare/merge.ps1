# echo30.ps1 결과 폴더의 조건마다 두 워커의 프로파일러를 합쳐 보여 준다(횟수는 더하고, 평균은 횟수로 가중, 최소·최대는 작은·큰 값)
# 예: .\tools\echo_compare\merge.ps1 -Root 실험폴더\aes_echo_pcb\30s
param([string]$Root)
foreach ($d in Get-ChildItem $Root -Directory | Sort-Object { [int]($_.Name -replace 's(\d+)_.*', '$1') })
{
	$acc = @{}
	foreach ($line in Get-Content "$($d.FullName)\profiler.txt" -Encoding Default)
	{
		if ($line -match '^\|\s*([^|]+?)\s*\|\s*(\d+)\s*\|\s*([\d.]+)us\s*\|\s*([\d.]+)us\s*\|\s*([\d.]+)us')
		{
			$k = $Matches[1]; $n = [int64]$Matches[2]; $avg = [double]$Matches[3]; $min = [double]$Matches[4]; $max = [double]$Matches[5]
			if (-not $acc[$k]) { $acc[$k] = @{ N = 0; Sum = 0.0; Min = 1e18; Max = 0 } }
			$acc[$k].N += $n; $acc[$k].Sum += $n * $avg; if ($min -lt $acc[$k].Min) { $acc[$k].Min = $min }; if ($max -gt $acc[$k].Max) { $acc[$k].Max = $max }
		}
	}
	$cli = Get-Content "$($d.FullName)\client.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' }
	$rates = $cli | ForEach-Object { if ($_ -match '에코/s=(\d+)') { [int]$Matches[1] } } | Select-Object -Skip 3
	$sorted = @($rates | Sort-Object); $med = if ($sorted.Count -gt 0) { $sorted[[int]($sorted.Count / 2)] } else { '?' }
	$last = $cli[-1]
	$err = if ($last -match '프로토콜오류=(\d+) 예상밖끊김=(\d+)') { "$($Matches[1])/$($Matches[2])" } else { '?' }
	$srvLast = (Get-Content "$($d.FullName)\server.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' })[-1]
	Write-Host "== $($d.Name)  에코/s 중앙값=$med  클라 프로토콜오류/예상밖끊김=$err"
	Write-Host "   $srvLast"
	foreach ($k in $acc.Keys | Sort-Object)
	{
		'{0,-26} n={1,9} avg={2,10:N2}us min={3,8:N2}us max={4,10:N1}us' -f $k, $acc[$k].N, ($acc[$k].Sum / $acc[$k].N), $acc[$k].Min, $acc[$k].Max | Write-Host
	}
}
