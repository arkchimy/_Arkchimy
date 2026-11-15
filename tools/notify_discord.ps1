param(
	[string]$Event = 'Notification'	# Notification: 권한 승인·입력 대기(유휴) / Stop: 응답을 마치고 다음 지시를 기다림
)
# Claude Code 훅 - 입력이 필요할 때 디스코드 채널에 알린다
# - 웹훅 URL은 환경 변수 DISCORD_WEBHOOK_URL에서 읽는다(.claude/settings.local.json의 env - 커밋되지 않는 파일). 없으면 아무것도 안 한다
# - stdin: Claude Code가 보내는 훅 입력 JSON(Notification은 message·notification_type, Stop은 transcript_path)
# - 알림 실패가 작업을 막으면 안 되므로 항상 exit 0
[Console]::InputEncoding = New-Object Text.UTF8Encoding($false)	# Claude Code는 UTF-8로 보낸다 - 기본 CP949면 한글이 깨짐 (README_ERRORS 1-19)
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)

$url = $env:DISCORD_WEBHOOK_URL
if ([string]::IsNullOrWhiteSpace($url))
{
	exit 0
}

# 대화 기록(JSONL)에서 마지막 응답 텍스트의 첫 줄 - 뒤에서부터 찾아 비어 있지 않은 첫 텍스트
function Get-LastReplyLine($path)
{
	if (-not $path -or -not (Test-Path -LiteralPath $path))
	{
		return ''
	}
	$lines = @(Get-Content -LiteralPath $path -Tail 300 -Encoding UTF8)
	for ($i = $lines.Count - 1; $i -ge 0; $i--)
	{
		try
		{
			$entry = $lines[$i] | ConvertFrom-Json
		}
		catch
		{
			continue
		}
		if ($entry.type -ne 'assistant' -or -not $entry.message.content)
		{
			continue
		}
		foreach ($part in @($entry.message.content))
		{
			if ($part.type -eq 'text' -and $part.text -and $part.text.Trim().Length -gt 0)
			{
				$first = ($part.text.Trim() -split "`n")[0].Trim()
				if ($first.Length -gt 200)
				{
					$first = $first.Substring(0, 200) + '…'
				}
				return $first
			}
		}
	}
	return ''
}

$json = $null
try
{
	$stdin = [Console]::In.ReadToEnd()
	if ($stdin.Trim().Length -gt 0)
	{
		$json = $stdin | ConvertFrom-Json
	}
}
catch
{
}

$project = Split-Path -Leaf $env:CLAUDE_PROJECT_DIR
if ($Event -eq 'Stop')
{
	$reply = ''
	if ($json)
	{
		$reply = Get-LastReplyLine $json.transcript_path
	}
	$content = "✅ **[$project]** 응답을 마치고 다음 지시를 기다리는 중"
	if ($reply)
	{
		$content += "`n> $reply"
	}
}
else
{
	$message = '입력을 기다리는 중'
	$kind = ''
	if ($json)
	{
		if ($json.message)
		{
			$message = $json.message
		}
		if ($json.notification_type)
		{
			$kind = " ($($json.notification_type))"
		}
	}
	$content = "🔔 **[$project]** Claude가 기다리는 중$kind`n$message"
}

try
{
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12	# PowerShell 5.1 기본값은 디스코드가 거부하는 옛 TLS
	$body = @{ content = $content } | ConvertTo-Json -Compress
	$bytes = [Text.Encoding]::UTF8.GetBytes($body)	# 문자열 그대로 넘기면 5.1이 UTF-8이 아닌 인코딩으로 보내 한글이 깨짐
	Invoke-RestMethod -Uri $url -Method Post -ContentType 'application/json; charset=utf-8' -Body $bytes -TimeoutSec 10 | Out-Null
}
catch
{
}
exit 0
