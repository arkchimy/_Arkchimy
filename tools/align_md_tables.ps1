param([Parameter(Mandatory = $true)][string]$Path)
# 마크다운 표를 화면 칸 너비(한글 2칸)에 맞춰 정렬한다 - 구분 줄도 열 너비만큼 '-'로
# 예: .\tools\align_md_tables.ps1 -Path portfolio\문서.md (파일을 바로 고친다). 표 한 줄은 80칸 안으로 - 넘으면 표를 나누거나 긴 설명을 표 밖으로
$enc = New-Object Text.UTF8Encoding($false)
$lines = [IO.File]::ReadAllLines($Path, $enc)

function W([string]$s)
{
	$w = 0
	foreach ($ch in $s.ToCharArray())
	{
		$c = [int]$ch
		if (($c -ge 0xAC00 -and $c -le 0xD7A3) -or ($c -ge 0x1100 -and $c -le 0x11FF) -or ($c -ge 0x3130 -and $c -le 0x318F) -or ($c -ge 0x4E00 -and $c -le 0x9FFF)) { $w += 2 } else { $w += 1 }
	}
	return $w
}
function Split-Row([string]$line)
{
	$t = $line.Trim()
	$t = $t.Substring(1, $t.Length - 2)	# 앞뒤 | 제거
	return @($t.Split('|') | ForEach-Object { $_.Trim() })
}

$out = New-Object System.Collections.Generic.List[string]
$i = 0
while ($i -lt $lines.Length)
{
	if (-not $lines[$i].TrimStart().StartsWith('|'))
	{
		$out.Add($lines[$i]); $i++; continue
	}
	$block = @()
	while ($i -lt $lines.Length -and $lines[$i].TrimStart().StartsWith('|')) { $block += $lines[$i]; $i++ }
	$indent = $block[0].Substring(0, $block[0].Length - $block[0].TrimStart().Length)
	$rows = @($block | ForEach-Object { , (Split-Row $_) })
	$colCnt = ($rows | ForEach-Object { $_.Count } | Measure-Object -Maximum).Maximum
	$widths = @(0) * $colCnt
	for ($r = 0; $r -lt $rows.Count; $r++)
	{
		if ($r -eq 1) { continue }	# 구분 줄
		for ($c = 0; $c -lt $rows[$r].Count; $c++) { $widths[$c] = [math]::Max($widths[$c], (W $rows[$r][$c])) }
	}
	for ($r = 0; $r -lt $rows.Count; $r++)
	{
		$cells = @()
		for ($c = 0; $c -lt $colCnt; $c++)
		{
			if ($r -eq 1) { $cells += ('-' * [math]::Max(3, $widths[$c])) ; continue }
			$v = if ($c -lt $rows[$r].Count) { $rows[$r][$c] } else { '' }
			$cells += $v + (' ' * ([math]::Max(3, $widths[$c]) - (W $v)))
		}
		$out.Add($indent + '| ' + ($cells -join ' | ') + ' |')
	}
}
[IO.File]::WriteAllLines($Path, $out, $enc)
