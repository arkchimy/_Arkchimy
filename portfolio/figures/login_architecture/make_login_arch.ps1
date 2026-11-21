$ErrorActionPreference = 'Stop'
$sp = $PSScriptRoot # SVG 그림이 이 폴더에 있다. 실행: 저장소 맨 위에서 .\portfolio\figures\login_architecture\make_login_arch.ps1
$path = 'D:\_Arkchimy\portfolio\로그인서버_구조.docx'
$word = New-Object -ComObject Word.Application
$word.Visible = $false
$doc = $word.Documents.Add()
$sel = $word.Selection
foreach ($id in -1, -2, -3, -49, -63)
{
	$st = $doc.Styles.Item($id)
	$st.Font.Name = '맑은 고딕'
}
$doc.Styles.Item(-1).Font.Size = 10
$doc.Styles.Item(-1).ParagraphFormat.SpaceAfter = 4
$doc.Styles.Item(-2).Font.Size = 14
$doc.Styles.Item(-3).Font.Size = 12
$doc.Styles.Item(-63).Font.Size = 18

function Add-Para([string]$text, [int]$style = -1, [bool]$bold = $false)
{
	$sel.Style = $doc.Styles.Item($style)
	$sel.Font.Bold = [int]$bold
	$sel.TypeText($text)
	$sel.TypeParagraph()
	$sel.Font.Bold = 0
}
function Add-Bullets([string[]]$items)
{
	foreach ($t in $items) { Add-Para $t -49 }
}
function Add-Table([object[]]$rows, [int[]]$widths)
{
	$sel.Style = $doc.Styles.Item(-1)
	$t = $doc.Tables.Add($sel.Range, $rows.Count, $rows[0].Count)
	$t.Borders.Enable = 1
	$t.Range.Font.Size = 9
	$t.Range.ParagraphFormat.SpaceAfter = 0
	for ($r = 0; $r -lt $rows.Count; $r++)
	{
		for ($c = 0; $c -lt $rows[$r].Count; $c++) { $t.Cell($r + 1, $c + 1).Range.Text = [string]$rows[$r][$c] }
	}
	$t.Rows.Item(1).Range.Font.Bold = 1
	$t.Rows.Item(1).Shading.BackgroundPatternColor = 15983321
	if ($widths) { for ($c = 0; $c -lt $widths.Count; $c++) { $t.Columns.Item($c + 1).Width = $widths[$c] } }
	$sel.EndKey(6) | Out-Null
	$sel.TypeParagraph()
}
function Add-Figure([string]$file, [string]$caption)
{
	$sel.Style = $doc.Styles.Item(-1)
	$sel.ParagraphFormat.Alignment = 1
	$pic = $sel.InlineShapes.AddPicture("$sp\$file")
	$pic.LockAspectRatio = -1
	$pic.Width = 450
	$sel.TypeParagraph()
	$sel.Font.Size = 9
	$sel.Font.Italic = 1
	$sel.TypeText($caption)
	$sel.TypeParagraph()
	$sel.Font.Italic = 0
	$sel.Font.Size = 10
	$sel.ParagraphFormat.Alignment = 0
}
function Add-DecisionBox([string]$title)
{
	Add-Para $title -1 $true
	$sel.Style = $doc.Styles.Item(-1)
	$t = $doc.Tables.Add($sel.Range, 2, 2)
	$t.Borders.Enable = 1
	$t.Cell(1, 1).Range.Text = '의견'
	$t.Cell(2, 1).Range.Text = '결정'
	$t.Columns.Item(1).Width = 50
	$t.Columns.Item(2).Width = 400
	$t.Columns.Item(1).Shading.BackgroundPatternColor = 15983321
	$t.Rows.Item(1).Height = 28
	$t.Rows.Item(2).Height = 28
	$sel.EndKey(6) | Out-Null
	$sel.TypeParagraph()
}

Add-Para '로그인 서버 - 구조와 최적화 과정 (2판)' -63
Add-Bullets @(
	'기준: 커밋 92113fb (2026-11-20). 함수 단위 흐름과 코드 링크는 design/로그인서버_흐름.docx(4판), 결정 기록은 design/암호화_구조_결정.docx',
	'2판에서 바뀐 것: 키 쌍 스레드를 없앴다(3-8). 서버 임시 키 256개는 main이 시작할 때 만들고, 5번 쓴 키는 돌려준 워커가 새로 만든다',
	'측정 PC: PC A = 노트북(성능·효율 코어 혼합, 정확성 시험만), PC B = 데스크톱 i5-8600(같은 종류 코어 6개, 성능 측정). 숫자마다 어느 PC인지 적었다'
)

Add-Para '1. 요약' -2
Add-Bullets @(
	'로그인 서버는 가입·로그인을 받아 세션 키를 만들고 Redis에 넘긴다. 클라는 그 세션 키로 게임 서버에 들어간다. 로그인 구간은 RSA 2048, 게임 구간은 세션 키로 AES-256-GCM이다',
	'스레드는 IOCP 워커 2개가 요청 하나를 끝까지 처리한다(RSA → 비밀번호 해시 → MySQL·Redis → 응답). 오래 걸리는 RSA 키 쌍(하나 약 55ms)은 main이 시작할 때 256개를 만들어 두고, 5번 쓴 키만 돌려준 워커가 새로 만든다',
	'지금 구조는 측정으로 정했다: 메시지마다 RSA를 쓰면 요청 시간의 96%가 비밀 키 연산(줄일 수 없는 2048비트 거듭제곱)이라, 줄일 수 있는 것은 연산 횟수뿐이었다 → RSA는 로그인 때만, 그 뒤는 대칭 키(메시지 하나 약 1,000배 빠름)',
	'같은 원칙(비싼 일은 횟수를 줄이거나 미리 한다)으로 키 쌍 미리 만들기·키 돌려 쓰기·서명 락 제거를 했고, 스레드 사이를 잇는 장치를 줄이려고 인증 스레드와 키 쌍 스레드를 지웠다'
)

Add-Para '2. 지금 구조' -2
Add-Para '2-1. 스레드' -3
Add-Figure 'fig1_threads.svg' '그림 1. 스레드와 공유 자원 (파란 상자 = 스레드, 흰 상자 = 공유 자원과 보호 방법)'
Add-Table @(
	@('스레드', '개수', '하는 일', '잡는 락'),
	@('IOCP 워커', '2 (workerCnt)', 'HelloServer·서명, 요청 확인·풀기, Argon2id, MySQL·Redis, 응답 잠그기·서명·보내기, 해제(5번 쓴 키는 새로 만듦, 약 55ms)', '키 풀 락·연결 풀 락 (꺼내고 넣을 때만, 키를 만드는 동안은 놓음)'),
	@('기한 감시', '1', '1초마다 요청 없는 연결(10초)·확인 없는 연결(5초)을 끊음', '없음 (atomic 읽기)'),
	@('main', '1', '설정·DB·Redis 준비, 서버 임시 키 256개 만들기(약 14초) → 서버 시작 → 종료', '키 풀 락 (넣을 때만)'),
	@('모니터', '1', '1초마다 통계 한 줄', '-')
) @(70, 60, 220, 100)
Add-Bullets @(
	'같은 연결의 콜백은 동시에 불리지 않는다(라이브러리 보장). 그래서 연결마다의 상태(실패 수, 통로의 번호)는 락이 없다. 기한 감시가 읽는 상태·시각만 atomic이다',
	'보내는 스레드가 그 연결의 워커 하나뿐이라 송신 락이 없다. 응답 번호가 순서대로 송신 링에 들어간다',
	'서버 고정 키 하나를 모든 워커가 락 없이 함께 서명한다. 근거는 시험뿐이다(3-3) - CNG 문서가 같은 키의 동시 사용을 보장하지는 않는다',
	'서버 임시 키는 풀에 남은 키 + 연결이 빌려 간 키 = 늘 256개다(새 키 만들기가 실패할 때만 줄어든다). 연결은 HelloServer부터 끊길 때까지 키를 쥐므로, 256은 HelloServer 뒤 아직 끊기지 않은 연결 수의 상한이다 - 다 빌려 가면 새 접속은 끊는다(3-8)'
)

Add-Para '2-2. 로그인 한 번' -3
Add-Figure 'fig3_sequence.svg' '그림 2. 로그인 한 번의 순서 (회색 = 워커가 하는 계산, 파란 글자 = 연결 상태)'
Add-Table @(
	@('단계', '시간', '어디서'),
	@('서버 임시 키 쌍 만들기', '약 55ms', 'main이 시작할 때 256개, 그 뒤 5번 쓴 키만 해제하는 워커가 (HelloServer 때는 꺼내기만)'),
	@('HelloClient 서명', '약 0.5ms', '워커'),
	@('요청 확인 + 풀기', '0.02 + 0.5ms', '워커'),
	@('Argon2id (64MiB·2회)', '약 63ms', '워커 - 일부러 느리게 한 것이라 줄이지 않는다'),
	@('응답 잠그기 + 서명', '0.02 + 0.5ms', '워커')
) @(150, 90, 210)
Add-Bullets @(
	'RSA 시간은 PC B RSA 에코 측정, Argon2id는 PC A 측정(LoginServer.exe --bench). 로그인 한 번에 워커는 약 65ms를 쓰고 그 대부분이 Argon2id다',
	'클라는 응답을 받으면 AuthAck를 보내고 끊는다. 서버는 세션 키를 보낸 뒤 먼저 끊지 않는다(3-7)'
)

Add-Para '2-3. 연결 상태' -3
Add-Figure 'fig4_state.svg' '그림 3. 연결 상태. 상태마다 받는 메시지가 정해져 있고 그 밖은 끊는다'
Add-Bullets @(
	'상태로 막는 것: HelloServer 전의 요청, 한 연결에서 두 번째 가입, 로그인 성공 뒤 다시 로그인(맞는 비밀번호로 해시를 계속 시키기), 세션 키를 받고도 확인을 보내지 않고 연결을 붙잡기',
	'Pending(워커가 처리 중)은 기한 감시가 끊지 않는다. 처리는 OnRecv 안에서 끝나므로 그 사이에 같은 연결의 다음 메시지를 받지 않는다'
)

Add-Para '3. 어떻게 여기까지 왔나' -2
Add-Para '모든 단계가 같은 질문에서 출발했다: 가장 비싼 일이 무엇이고, 그 횟수를 줄이거나 미리 할 수 있는가. 아래 표가 순서이고, 단계마다 문제 → 근거 → 결정 → 결과를 적었다.'
Add-Table @(
	@('단계', '문제', '결정', '근거·결과'),
	@('3-1 (11-16)', '암호 세 가지(X25519·Ed25519·XChaCha20)를 직접 설명하기 어려움', '로그인 구간을 RSA 2048 하나로', '키 교환·서명·잠그기를 한 원리로 설명'),
	@('3-2 (11-17)', '접속마다 키 쌍(약 50ms)을 만들면 접속 폭주에 정상 로그인이 막힘', '키 쌍 스레드가 미리 64개', '정상 로그인 5개: 10.1초 실패 → 0.36초 성공 (PC A)'),
	@('3-3 (11-18)', '서명이 고정 키 락을 기다림', '서명 락 제거', '서명 1.04 → 0.78ms, 에코 +19% (PC A)'),
	@('3-4 (11-19)', '메시지마다 RSA면 서버 코어 하나에 초당 약 850 메시지', '로그인만 RSA, 게임은 AES-256-GCM', '메시지 하나 1.17ms 대 1.16µs, 약 1,000배 (PC B)'),
	@('3-5 (11-19)', '키 쌍 스레드 하나의 한계 초당 약 18개', '키를 돌려받아 5연결까지', '접속 670에 키 생성 244 (시작 64 포함, PC A)'),
	@('3-6 (11-19)', '인증 스레드로 넘기는 장치 4개, 해시를 동시에 1개', '워커가 끝까지 처리', '장치 4개 삭제, 해시 동시 실행 = 워커 수'),
	@('3-7 (11-19)', '서버가 먼저 RST로 끊으면 클라가 아직 안 읽은 응답이 사라짐', '연결 상태 + AuthAck', '1시간 시험 로그인 472 = 확인 472 (PC A)'),
	@('3-8 (11-20)', '키 돌려 쓰기 뒤에는 새 키가 5번째 반납 때만 필요한데 스레드를 따로 둠', '키 쌍 스레드 삭제, 키 256개, 없으면 끊기', '폭주 200개 뒤 정상 로그인 0.7초, 400개는 바로 끊고 기한 뒤 회복 (PC A)')
) @(62, 140, 110, 138)

Add-Para '3-1. 암호를 RSA 하나로' -3
Add-Bullets @(
	'문제: 처음에는 X25519(키 교환) + Ed25519(서명) + XChaCha20-Poly1305(잠그기)를 libsodium으로 썼다. 동작은 했지만 면접에서 원리를 직접 설명할 수 없는 기술은 쓰지 않기로 했다',
	'결정: 로그인 구간을 RSA 2048 하나로 바꿨다(CNG). 클라와 서버가 임시 키 쌍을 하나씩 주고받고, 요청은 OAEP로 잠그고 PSS로 서명한다. 서버 고정 키는 서명만 해서, 고정 키가 새도 녹화된 통신은 풀리지 않는다'
)
Add-Para '3-2. 키 쌍을 미리 만든다' -3
Add-Bullets @(
	'문제: RSA 키 쌍 만들기는 하나에 약 50ms다. 접속마다 그 자리에서 만들면 HelloServer만 보내는 접속 200개가 몰렸을 때 정상 로그인 5개가 10.1초 동안 모두 실패했다 (PC A)',
	'결정: 키 쌍 스레드가 64개를 미리 채우고 워커는 꺼내기만 한다. 같은 시험에서 정상 로그인 5개가 0.36초에 성공했다',
	'남은 한계: 키 쌍 스레드 하나는 초당 약 18개를 만든다. 그보다 빠른 접속이 계속되면 풀이 비어 새 접속을 끊는다(통계 키쌍부족끊김) → 3-5에서 만드는 횟수를 줄였고, 3-8에서 스레드를 없앴다'
)
Add-Para '3-3. RSA를 재 보니 96%가 비밀 키 연산' -3
Add-Bullets @(
	'측정: 로그인 서버와 같은 RSA 흐름으로 메시지마다 에코하는 서버(RsaEcho)를 만들어 구간별 시간을 쟀다. 요청 하나 1.18ms 중 풀기 0.56 + 서명 0.57ms, 비밀 키 연산이 96%였다 (PC B, 37분, 346만 번)',
	'발견: 처음 잰 값에서 서명이 풀기보다 길었다(1.04 대 0.81ms, PC A). 모든 워커가 서버 고정 키 하나로 서명하며 키 락을 기다린 시간이었다',
	'결정: 서명만 락을 뺐다. 서명 1.04 → 0.78ms(풀기와 같아짐), 에코 +19% (PC A, 30초). 락 없이 1시간 시험 서명 383만 번에 클라 확인 실패 0',
	'한계: CNG 문서는 같은 키 핸들의 동시 사용을 보장하지 않는다. 근거는 시험이고, 조건은 서명하는 동안 키를 바꾸지 않는 것이다(시작할 때 한 번 읽음)'
)
Add-Para '3-4. 로그인 뒤에는 대칭 키' -3
Add-Bullets @(
	'질문: 비밀 키 연산은 2048비트 거듭제곱 자체라 코드로 줄일 수 없다. 줄일 수 있는 것은 연산 횟수뿐이다',
	'측정: RSA 에코와, 로그인만 RSA로 하고 에코는 세션 키로 AES-256-GCM을 쓰는 서버(AesEcho)를 같은 조건으로 30분씩 쟀다 (PC B, 세션 64)'
)
Add-Table @(
	@('항목', 'RSA (메시지마다)', 'AES-256-GCM', '차이'),
	@('서버 메시지 하나', '1.17ms', '1.16µs', '약 1,000배'),
	@('서버 CPU ÷ 처리한 메시지', '1.19ms', '1.21µs', '약 990배'),
	@('서버 CPU (워커 2)', '1.88코어 (포화)', '0.17코어', ''),
	@('오류', '0 (282만 번)', '0 (2억 6천만 번)', '')
) @(140, 110, 110, 90)
Add-Bullets @(
	'프로파일러 값과 프로세스 CPU 기록이 같은 배수라 측정 도구 탓이 아니다. 메시지마다 RSA면 서버 코어 하나가 초당 약 850 메시지라 게임 서버에 쓸 수 없다',
	'결정: 로그인 서버만 RSA, 게임 메시지는 로그인 서버가 만든 세션 키로 AES-256-GCM(CNG). 측정한 그 코드를 게임 서버에 옮겼다(XChaCha20 대신)'
)
Add-Para '3-5. 서버 임시 키를 돌려 쓴다' -3
Add-Bullets @(
	'문제: 3-2의 한계(초당 약 18개). RSA 에코 측정에서 키를 5세션까지 다시 쓰면 키 만들기가 접속의 20%로 줄었다',
	'결정: 미리 만들기는 그대로 두고, 연결이 끝나면(OnRelease) 키를 돌려받아 5연결까지 다시 빌려준다. 워커는 키를 만들지 않는다 - 접속 폭주에서 워커가 막히지 않게',
	'결과: HelloServer만 보내는 접속 400개 시험에서 수락 670에 키 생성 244(시작 때 64 포함), 그 뒤 정상 로그인 5개 0.75초 (PC A)',
	'대가: 키 하나가 새면 그 키를 쓴 연결(최대 5개)의 가입·로그인 요청이 함께 풀린다. server.cfg loginKeyMaxUse = 1이면 접속마다 새 키로 돌아간다',
	'이때 숫자는 키 쌍 스레드가 있던 구조(키 64개)의 값이다. 다음 날 스레드를 없앴다(3-8)'
)
Add-Para '3-6. 인증 스레드를 없애고 워커가 처리한다' -3
Add-Figure 'fig2_change.svg' '그림 4. 바뀌기 전(인증 스레드에 넘김)과 지금(워커가 끝까지 처리)'
Add-Bullets @(
	'문제: 처음에는 워커가 RSA만 풀고 해시·MySQL·Redis는 인증 스레드 하나에 넘겼다(블록되는 일을 워커 밖으로). 그러려면 스레드 사이를 잇는 장치 4개가 필요했다 - 처리 중 해제를 막는 HoldSession, 대기열 상한(200), 이미 끊긴 요청 건너뛰기, 워커와 인증 스레드가 함께 보내므로 세션 송신 락',
	'근거: RSA 에코 서버는 워커가 RSA와 처리를 그 자리에서 끝내는 구조로 1시간 시험을 오류 없이 돌았다. 인증 스레드는 하나라 해시(63ms)가 한 번에 하나씩만 돈다(계산상 초당 약 16 로그인)',
	'결정: 인증 스레드를 지우고 워커가 OnRecv 안에서 끝까지 처리한다. DB·Redis 연결은 한 번에 한 스레드만 쓰게 만들어져 있어 워커 수만큼 연결 쌍을 두고 요청 하나 동안 빌린다(LoginStorePool). 장치 4개가 모두 없어졌고, 해시는 워커 수(2)만큼 동시에 돈다(계산상 약 32)',
	'대가(시험으로 확인): 해시·DB를 기다리는 동안 그 워커는 다른 연결을 받지 못한다. DB 테이블을 6초 잠근 시험에서 그동안 새로 연 접속의 HelloClient가 3.9초 늦었다 (PC A, login_edge TC-L10). 통계 처리중워커가 워커 수와 같으면 이 상태다',
	'처리량을 두 구조로 비교해 재지는 않았다. 위 초당 로그인 수는 해시 시간으로 계산한 값이다'
)
Add-Para '3-7. 연결 상태와 AuthAck' -3
Add-Bullets @(
	'문제: 라이브러리는 연결을 linger 0(RST)으로 닫는다. 서버가 응답 직후 끊으면, 클라 수신 버퍼에 이미 도착한 응답도 클라가 읽기 전이면 버려진다 - 루프백 시험 5회 모두 클라가 0바이트를 읽었다 (PC A)',
	'결정: 세션 키를 보낸 뒤 서버는 먼저 끊지 않고 클라의 AuthAck를 기다린다(5초). AuthAck도 다른 메시지처럼 잠그고 서명해 누가 끼워 넣을 수 없다. 함께 연결 상태(그림 3)를 두어 상태에 맞지 않는 메시지는 끊는다',
	'결과: 1시간 시험(봇 30)에서 로그인 472 = 확인받음 472, 확인시간초과 0 (PC A)'
)
Add-Para '3-8. 키 쌍 스레드를 없앤다' -3
Add-Bullets @(
	'문제: 3-5로 키를 돌려 쓰게 된 뒤로, 새 키가 필요한 때는 5번 쓴 키가 돌아올 때뿐이다. 그런데도 키 쌍 스레드와, 꺼낼 때 그 스레드를 깨우는 조건 변수를 따로 두고 있었다',
	'결정(사용자): 키 쌍 스레드를 지운다. main이 시작할 때 키를 만들고, 5번 쓴 키는 돌려준 워커가 OnRelease에서 새로 만든다(약 55ms, 키를 만드는 동안은 풀 락을 놓음). 키가 떨어지면 그 자리에서 만들지 않고 끊는다 - HelloServer마다 워커가 키를 만들면 3-2의 문제(폭주에 워커가 키만 만듦)가 돌아온다',
	'결과로 생긴 제약: 풀에 남은 키 + 연결이 빌려 간 키가 늘 키 수와 같아, 키 수가 HelloServer 뒤 아직 끊기지 않은 연결 수의 상한이 된다. 키 64개로는 HelloServer만 보내는 접속 200·400개 뒤 정상 로그인이 0개였다(login_flood 2개 실패, PC A)',
	'결정(사용자): 키 수를 256개로 늘린다(시작 약 14초 늦어짐). 결과: 폭주 200개 뒤 정상 로그인 5개 0.6–0.7초 성공, 400개면 새 접속이 0.14초에 끊기고 요청 기한(10초)이 지난 뒤 0.47초에 회복, 끝나면 키 256개 모두 돌아옴. run_all 8·login_edge 43 통과 (PC A). 1시간을 채운 장시간 시험은 아직이다',
	'대가: 5번째 연결마다 해제하는 워커가 약 55ms 막힌다. HelloServer만 보낸 접속 256개를 10초마다 새로 열면 로그인이 계속 막힌다(주소별 제한 없음)'
)

Add-Para '4. 대가와 한계' -2
Add-Bullets @(
	'워커 블록: 해시·DB·Redis를 기다리는 워커는 다른 연결을 처리하지 못한다. DB가 멈추면 그동안 새 접속도 늦다(3-6)',
	'키 돌려 쓰기: 키 하나가 새면 최대 5연결의 요청이 풀린다(3-5)',
	'락 없는 서명: 근거가 시험뿐이다(3-3)',
	'키 수 상한: 키 256개를 모두 빌려 가면 새 접속은 끊는다. HelloServer만 보낸 접속 256개를 10초마다 새로 열면 로그인이 계속 막힌다. 주소별 접속 제한은 없다(3-8)',
	'해제 때 키 만들기: 5번째 연결마다 해제하는 워커가 약 55ms 막힌다. 서버를 끌 때도 남은 연결을 해제하며 키를 만들어 종료가 늦어진다(곧 지울 키라 쓸모없음)',
	'5번째 로그인 실패: 실패 응답 직후 끊으므로 응답이 RST에 사라질 수 있다. 실패 알림이라 클라는 끊김으로 처리한다',
	'측정 환경: 서버와 클라가 같은 PC(루프백)다'
)

Add-Para '5. 근거 문서와 원본' -2
Add-Bullets @(
	'design/로그인서버_흐름.docx(4판) - 함수 단위 흐름(Ⓐ–⑪), 상태별로 끊는 경우 표, 코드 링크',
	'design/암호화_구조_결정.docx - 3-4·3-5·3-3을 정한 결정 기록(의견·결정 칸)',
	'design/RSA_AES에코_비교.docx, 실험폴더/aes_echo_pcb_2026_11_19/ - 3-4 측정 원본',
	'design/RSA에코_흐름.docx, 실험폴더/rsa_echo_pcb_2026_11_18/ - 3-3 측정 원본',
	'design/login_rsa_change.docx - 3-1·3-2 결정 기록'
)

Add-Para '6. 남은 결정' -2
Add-DecisionBox '6-1. 흐름 문서(4판)를 어떻게 둘지 (추천: 포트폴리오 첫 문서는 이 문서, 흐름 문서는 코드 링크가 있는 상세 자료로 그대로 둔다)'
Add-DecisionBox '6-2. portfolio/README.md에 어떻게 연결할지 (추천: "1. 구조"에 "로그인 서버 구조" 한 줄 추가)'

$doc.SaveAs2($path, 16)
$doc.ExportAsFixedFormat("$env:TEMP\login_arch_check.pdf", 17)
"쪽 수: $($doc.ComputeStatistics(2)) 그림: $($doc.InlineShapes.Count) 표: $($doc.Tables.Count)"
$doc.Close()
$word.Quit()
