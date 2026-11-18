param([string]$Root)
$tags = 'Handshake total','Handshake sign','Request total','Request decrypt: verify','Request decrypt: OAEP','Response encrypt: OAEP','Response encrypt: sign','Response send','Echo logic'
foreach ($d in Get-ChildItem $Root -Directory | Sort-Object { [int]($_.Name -replace 's(\d+)_.*','$1') })
{
	$acc = @{}
	foreach ($line in Get-Content "$($d.FullName)\profiler.txt" -Encoding Default)
	{
		if ($line -match '^\|\s*([^|]+?)\s*\|\s*(\d+)\s*\|\s*([\d.]+)us\s*\|\s*([\d.]+)us')
		{
			$k = $Matches[1]; $n = [int64]$Matches[2]; $avg = [double]$Matches[3]; $min = [double]$Matches[4]
			if (-not $acc[$k]) { $acc[$k] = @{ N = 0; Sum = 0.0; Min = 1e18 } }
			$acc[$k].N += $n; $acc[$k].Sum += $n * $avg; if ($min -lt $acc[$k].Min) { $acc[$k].Min = $min }
		}
	}
	$cli = Get-Content "$($d.FullName)\client.txt" -Encoding Default | Where-Object { $_ -match '\[stat\]' }
	$rates = $cli | ForEach-Object { if ($_ -match '에코/s=(\d+)') { [int]$Matches[1] } } | Select-Object -Skip 3
	$sorted = $rates | Sort-Object; $med = $sorted[[int]($sorted.Count / 2)]
	$last = $cli[-1]
	$err = if ($last -match '프로토콜오류=(\d+) 예상밖끊김=(\d+)') { "$($Matches[1])/$($Matches[2])" } else { '?' }
	Write-Host "== $($d.Name)  에코/s 중앙값=$med  프로토콜오류/예상밖끊김=$err"
	foreach ($t in $tags)
	{
		if ($acc[$t]) { '{0,-26} n={1,9} avg={2,9:N3}ms min={3,7:N3}ms' -f $t, $acc[$t].N, ($acc[$t].Sum / $acc[$t].N / 1000), ($acc[$t].Min / 1000) | Write-Host }
	}
}
