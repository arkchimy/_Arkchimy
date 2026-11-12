param([uint32]$TargetPid, [string]$LogPath = "", [string]$Key = "q")
# 대상 프로세스의 콘솔 입력 버퍼에 키 이벤트 하나를 넣는다 - 키로만 조작하는 에코 서버·클라(S, Q)를 시험 스크립트에서 조작할 때
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class QKeyInput {
    [StructLayout(LayoutKind.Explicit, CharSet=CharSet.Unicode)]
    public struct INPUT_RECORD {
        [FieldOffset(0)] public ushort EventType;
        [FieldOffset(4)] public int bKeyDown;
        [FieldOffset(8)] public ushort wRepeatCount;
        [FieldOffset(10)] public ushort wVirtualKeyCode;
        [FieldOffset(12)] public ushort wVirtualScanCode;
        [FieldOffset(14)] public char UnicodeChar;
        [FieldOffset(16)] public uint dwControlKeyState;
    }
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool WriteConsoleInputW(IntPtr h, INPUT_RECORD[] recs, uint len, out uint written);
}
"@
function Write-Log([string]$msg) { if ($LogPath -ne "") { $msg | Out-File -FilePath $LogPath -Encoding utf8 -Append } }
[QKeyInput]::FreeConsole() | Out-Null
if (-not [QKeyInput]::AttachConsole($TargetPid)) { Write-Log "AttachConsole 실패 err=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }
$h = [QKeyInput]::CreateFileW("CONIN$", [uint32]3221225472, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
$recs = New-Object QKeyInput+INPUT_RECORD[] 2
foreach ($i in 0, 1) { $r = New-Object QKeyInput+INPUT_RECORD; $r.EventType = 1; $r.bKeyDown = 1 - $i; $r.wRepeatCount = 1; $r.wVirtualKeyCode = [uint16][char]$Key.ToUpper(); $r.UnicodeChar = [char]$Key; $recs[$i] = $r } # 구조체 배열 원소의 필드에 바로 쓰면 복사본만 바뀐다
$written = 0
$ok = [QKeyInput]::WriteConsoleInputW($h, $recs, 2, [ref]$written)
Write-Log "WriteConsoleInput=$ok written=$written"
exit 0