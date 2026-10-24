param([uint32]$TargetPid, [string]$LogPath = "")

# 대상 프로세스의 콘솔에 붙어 Ctrl+C를 보낸다 - 자신은 핸들러로 무시
# 서버가 콘솔 키 입력 대신 Ctrl+C로 종료하므로, 백그라운드로 띄운 서버를 정상 종료시킬 때 쓴다
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ConsoleCtrl {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint groupId);
}
"@

function Write-Log([string]$msg) {
    if ($LogPath -ne "") { $msg | Out-File -FilePath $LogPath -Encoding utf8 }
}

[ConsoleCtrl]::FreeConsole() | Out-Null
if (-not [ConsoleCtrl]::AttachConsole($TargetPid)) {
    Write-Log "AttachConsole 실패 err=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    exit 1
}
[ConsoleCtrl]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
$ok = [ConsoleCtrl]::GenerateConsoleCtrlEvent(0, 0)
Write-Log "GenerateConsoleCtrlEvent=$ok"
Start-Sleep -Milliseconds 500
exit 0
