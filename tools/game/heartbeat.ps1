# 하트비트와 입장 뒤 유휴 기한 - 하트비트를 보내는 클라는 채팅 없이도 남고, 멈춘 클라는 서버가 끊는다
# idleLimitSec를 시험 동안 12초로(클라 하트비트 10초보다 커야 한다)
. "$PSScriptRoot\GameTestCommon.ps1"
New-TestAccount 'hb_alive'
New-TestAccount 'hb_silent'

$orig = Set-ServerCfg 'idleLimitSec' 12
try
{
	Start-GameServer 'heartbeat'
	$a = New-GameClient 'a'; Say $a 'login hb_alive' 1500
	$s = New-GameClient 's'; Say $s 'login hb_silent' 1500
	Say $s 'hbstop' 300           # 첫 하트비트(접속 10초 뒤) 전에 멈춘다 - 입장 뒤 아무것도 보내지 않는다
	Start-Sleep 20                # 유휴 기한 12초 + 감시 1초 + 여유. a는 그동안 하트비트만 보낸다(10초마다)
	Say $a 'chat still-here' 1000 # a가 아직 입장해 있으면 자기 채팅이 돌아온다
	Start-Sleep -Milliseconds 1200
	$idle = Get-ServerStat '무응답끊김'
	$bad = Get-ServerStat '잘못된패킷'
	$outA = Close-GameClient $a
	$outS = Close-GameClient $s
	Stop-GameServer | Out-Null
}
finally
{
	Restore-ServerCfg $orig
}

"===== 판정"
Test-Check '하트비트를 보내는 클라는 20초 동안 채팅 없이도 남음(그 뒤 채팅이 돌아옴)' (($outA -match '\[채팅\] hb_alive: still-here') -and -not ($outA -match '연결 끊김'))
Test-Check '하트비트를 멈춘 클라는 서버가 끊음' ($outS -match '연결 끊김')
Test-Check '서버: 무응답끊김 1, 잘못된패킷 0(하트비트를 받아들임)' ($idle -eq 1 -and $bad -eq 0) "무응답끊김 $idle, 잘못된패킷 $bad"
Write-Verdict 'heartbeat'
