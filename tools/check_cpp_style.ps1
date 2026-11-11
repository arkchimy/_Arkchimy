# C++ 코딩 규칙 중 기계로 확인할 수 있는 것만 검사한다 (CODING_STANDARD.md). 파일은 고치지 않는다
# - 훅(PostToolUse, Edit/Write): stdin JSON의 tool_input.file_path 하나를 검사, 위반이면 stderr + exit 2 -> Claude에게 전달
# - 직접 실행: .\tools\check_cpp_style.ps1 -Path 파일1,파일2 (없으면 범위 안 전체)
# 이름 규칙처럼 판단이 필요한 것은 cpp-reviewer가 본다
param(
	[string[]]$Path
)

$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = New-Object Text.UTF8Encoding($false)	# Claude Code는 UTF-8로 주고받는다 - 기본 CP949면 한글 경로·메시지가 깨짐
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)

$repoRoot = Split-Path -Parent $PSScriptRoot

# 적용 범위: _lib/NetworkLib/, _lib/ProtocolLib/, 컨텐츠 폴더의 Server/·Protocol/(예: Echo/Server/)의 .h/.cpp. Client/는 범위 밖
function Test-InScope([string]$file)
{
	$p = $file -replace '/', '\'
	return ($p -match '\\(_lib\\NetworkLib|_lib\\ProtocolLib|[^\\]+\\Server|[^\\]+\\Protocol)\\[^\\]+\.(h|cpp)$')
}

# 주석과 문자열 안의 글자를 검사에서 빼기 위해 지운다(블록 주석 여러 줄은 단순하게 처리)
function Remove-CommentsAndStrings([string]$line, [ref]$inBlock)
{
	$s = $line
	if ($inBlock.Value)
	{
		$end = $s.IndexOf('*/')
		if ($end -lt 0)
		{
			return ''
		}
		$s = $s.Substring($end + 2)
		$inBlock.Value = $false
	}
	$s = $s -replace '"([^"\\]|\\.)*"', '""'
	$s = $s -replace "'([^'\\]|\\.)*'", "''"
	$s = $s -replace '/\*.*?\*/', ''
	$start = $s.IndexOf('/*')
	if ($start -ge 0)
	{
		$s = $s.Substring(0, $start)
		$inBlock.Value = $true
	}
	$s = $s -replace '//.*$', ''
	return $s
}

function Get-Violations([string]$file)
{
	$result = New-Object System.Collections.Generic.List[string]
	$name = Split-Path -Leaf $file

	$bytes = [IO.File]::ReadAllBytes($file)
	if ($bytes.Length -lt 3 -or $bytes[0] -ne 0xEF -or $bytes[1] -ne 0xBB -or $bytes[2] -ne 0xBF)
	{
		$result.Add("${name}: BOM 없음 - C++ 소스·헤더는 BOM 있는 UTF-8 (없으면 MSVC가 CP949로 읽음)")
	}

	$lines = [IO.File]::ReadAllLines($file, (New-Object Text.UTF8Encoding($false)))

	if ($name -like '*.h')
	{
		$hasPragma = $false
		foreach ($l in $lines)
		{
			if ($l -match '^\s*#pragma\s+once\b')
			{
				$hasPragma = $true
				break
			}
		}
		if (-not $hasPragma)
		{
			$result.Add("${name}: #pragma once 없음")
		}
	}

	# 스레드 공유 변수(volatile)는 Interlocked 함수로만 쓴다 - 같은 폴더 헤더에서 선언된 멤버 이름(m 접두어)을 모은다(CODING_STANDARD)
	# 구조체 필드(WorkerCounter의 PascalCase)는 뺀다 - 단일 쓰기 예외(AddSingleWriter)이고, 같은 이름의 일반 필드(NetStat)가 있어 오탐이 난다
	$volatileNames = @()
	foreach ($h in Get-ChildItem -Path (Split-Path -Parent $file) -Filter *.h -File)
	{
		foreach ($m in [regex]::Matches([IO.File]::ReadAllText($h.FullName), 'volatile\s+LONG(?:64)?\s+(m\w+)'))
		{
			$volatileNames += $m.Groups[1].Value
		}
	}
	$volatileNames = @($volatileNames | Sort-Object -Unique)

	$bMemoryWrapper = ($name -ceq 'Memory.h')	# 표준 함수와 new/delete를 감싸는 곳이라 금지 목록에서 뺀다
	$inBlock = $false
	for ($i = 0; $i -lt $lines.Length; ++$i)
	{
		$raw = $lines[$i]
		$no = $i + 1

		if ($raw -match '^ +[^\s*]')
		{
			$result.Add("${name}:${no}: 공백 들여쓰기 - 탭으로")
		}

		$code = Remove-CommentsAndStrings $raw ([ref]$inBlock)
		$trim = $code.Trim()

		# 여는 중괄호는 항상 새 줄. 중괄호 초기화(= {, ({, , {)는 제외
		if ($trim.EndsWith('{') -and $trim -cne '{' -and $trim -notmatch '(=|\(|,)\s*\{$')
		{
			$result.Add("${name}:${no}: 여는 중괄호가 같은 줄 - 새 줄로")
		}

		if ($volatileNames.Count -gt 0 -and $code -cnotmatch '\bvolatile\b')	# 선언 줄(초기값 = 0)은 제외
		{
			foreach ($v in $volatileNames)
			{
				if ($code -cmatch "(\+\+|--)\s*(?:[\w\]\)]+(?:\.|->))?$v\b" -or $code -cmatch "\b$v\s*(\+\+|--|\+=|-=|=(?!=))")
				{
					$result.Add("${name}:${no}: volatile 변수 $v 를 일반 쓰기(=, ++, --, +=) - Interlocked 함수로")
				}
			}
		}

		if (-not $bMemoryWrapper)
		{
			if ($code -cmatch '\b(memcpy|memset|memmove|ZeroMemory|CopyMemory)\b')
			{
				$result.Add("${name}:${no}: $($Matches[1]) 금지 - MemCpy/MemSet/MemMove")
			}
			if ($code -cmatch 'std::make_unique\b')
			{
				$result.Add("${name}:${no}: std::make_unique 금지 - MakeUnique/MakeUniqueArray")
			}
			if ($code -cmatch '\bnew\s+[A-Za-z_]')
			{
				$result.Add("${name}:${no}: new 직접 사용 금지 - New/NewArray")
			}
			if ($code -cmatch '\bdelete\s*(\[\s*\])?\s*[A-Za-z_(]' -and $code -cnotmatch '=\s*delete\b')
			{
				$result.Add("${name}:${no}: delete 직접 사용 금지 - Delete/DeleteArray")
			}
		}
		if ($code -cmatch '\bNULL\b')
		{
			$result.Add("${name}:${no}: NULL 금지 - nullptr")
		}
	}
	return $result
}

# 검사할 파일 정하기
$files = @()
$bHook = $false
if ($Path)
{
	$files = $Path
}
else
{
	$stdin = ''
	if ([Console]::IsInputRedirected)
	{
		$stdin = [Console]::In.ReadToEnd()
	}
	if ($stdin.Trim().Length -gt 0)
	{
		$bHook = $true
		try
		{
			$json = $stdin | ConvertFrom-Json
			$files = @($json.tool_input.file_path)
		}
		catch
		{
			exit 0	# 입력을 못 읽으면 작업을 막지 않는다
		}
	}
}
if (-not $bHook -and -not $Path)
{
	$dirs = @(@('_lib\NetworkLib', '_lib\ProtocolLib') | ForEach-Object { Join-Path $repoRoot $_ }) +
		@(Get-ChildItem -Path $repoRoot -Directory | ForEach-Object { Join-Path $_.FullName 'Server'; Join-Path $_.FullName 'Protocol' }) |
		Where-Object { Test-Path $_ }
	$files = Get-ChildItem -Path $dirs -File |
		Where-Object { $_.Extension -in '.h', '.cpp' } | ForEach-Object { $_.FullName }
}

$all = New-Object System.Collections.Generic.List[string]
foreach ($f in $files)
{
	if (-not $f -or -not (Test-InScope $f) -or -not (Test-Path -LiteralPath $f))
	{
		continue
	}
	foreach ($v in (Get-Violations $f))
	{
		$all.Add($v)
	}
}

if ($all.Count -eq 0)
{
	if (-not $bHook)
	{
		Write-Output "위반 없음 ($($files.Count)개 파일)"
	}
	exit 0
}

$maxShow = 20
$msg = New-Object System.Text.StringBuilder
[void]$msg.AppendLine("코딩 규칙 위반 $($all.Count)건 (CODING_STANDARD.md, tools/check_cpp_style.ps1):")
foreach ($v in ($all | Select-Object -First $maxShow))
{
	[void]$msg.AppendLine("  $v")
}
if ($all.Count -gt $maxShow)
{
	[void]$msg.AppendLine("  ... 외 $($all.Count - $maxShow)건")
}
[Console]::Error.Write($msg.ToString())
exit 2
