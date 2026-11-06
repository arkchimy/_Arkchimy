$ErrorActionPreference = 'Stop'
$repo = 'C:\_ArkchimyProject'
$wt = 'C:\_Arkchimy_deadlock'
Copy-Item "$repo\_lib\NetworkLib\NetworkLib.cpp" "$wt\_lib\NetworkLib\NetworkLib.cpp" -Force
Copy-Item "$repo\_lib\NetworkLib\NetworkLib.h" "$wt\_lib\NetworkLib\NetworkLib.h" -Force
$bom = New-Object Text.UTF8Encoding($true)
$noBom = New-Object Text.UTF8Encoding($false)

function Rep([string]$text, [string]$old, [string]$new) {
    $n = ([regex]::Matches($text, [regex]::Escape($old))).Count
    if ($n -ne 1) { throw "anchor count $n : $old" }
    return $text.Replace($old, $new)
}

# library test code
$p = "$wt\_lib\NetworkLib\NetworkLib.cpp"
$t = [IO.File]::ReadAllText($p)
$t = Rep $t 'static thread_local int tWorkerIdx = -1;' "#include <cstdio> // [test]`r`nstatic volatile LONG64 gDeferTestCnt[3] = {}; // [test] 0: in SendPacket, 1: other worker path, 2: non-worker`r`nstatic thread_local bool tbInSendPacketTest = false; // [test]`r`nstatic thread_local int tWorkerIdx = -1;"
$t = Rep $t "const bool bSent = marshalToSendBuf(*session, payload, len);`r`n`tdecIoCount(*session);" "const bool bSent = marshalToSendBuf(*session, payload, len);`r`n`ttbInSendPacketTest = true; // [test]`r`n`tdecIoCount(*session);"
$t = Rep $t "`treturn bSent;" "`ttbInSendPacketTest = false; // [test]`r`n`treturn bSent;"
$t = Rep $t 'if (!PostQueuedCompletionStatus(mIOCP, 0, reinterpret_cast<ULONG_PTR>(&session), &session.mReleaseOv))' 'if (true) // [test] force failure'
$t = Rep $t '// LOG : PQCS 실패 - 그래도 슬롯은 돌려줘야 한다' "InterlockedIncrement64(&gDeferTestCnt[tbInSendPacketTest ? 0 : (tWorkerIdx >= 0 ? 1 : 2)]); // [test]"
$t = Rep $t '// 수락 대기가 0이 된 뒤에 지운다' "printf(`"[defer-test] inSendPacket=%lld otherWorker=%lld nonWorker=%lld\n`", gDeferTestCnt[0], gDeferTestCnt[1], gDeferTestCnt[2]); fflush(stdout); // [test]`r`n`t// 수락 대기가 0이 된 뒤에 지운다"
[IO.File]::WriteAllText($p, $t, $bom)

# content test code: re-entry marker + exception print
$p = "$wt\networkLibProject\ChatServer.h"
$t = [IO.File]::ReadAllText($p)
$t = Rep $t "#include <mutex>`r`n" "#include <mutex>`r`n#include <system_error>`r`n"
$t = Rep $t "#pragma comment(lib, `"winmm.lib`")`t// timeGetTime`r`n" "#pragma comment(lib, `"winmm.lib`")`t// timeGetTime`r`n`r`nstatic thread_local bool tbInBroadcastLock = false; // [test]`r`n"
$t = Rep $t "void OnRelease(const uint64_t id) override`r`n`t{`r`n" "void OnRelease(const uint64_t id) override`r`n`t{`r`n`t`tif (tbInBroadcastLock) { printf(`"[deadlock-test] 방 락 안에서 OnRelease 재진입\n`"); } // [test]`r`n"
$t = Rep $t "leave(roomIdx, id, false);`t// 끊긴 세션에는 LeaveOk를 보낼 곳이 없다" "try { leave(roomIdx, id, false); } catch (const std::system_error& e) { printf(`"[deadlock-test] leave 예외 inLock=%d code=%d what=%s\n`", tbInBroadcastLock ? 1 : 0, e.code().value(), e.what()); throw; } // [test]"
$t = Rep $t "for (const uint64_t member : room.Members)`r`n`t`t`t{`r`n`t`t`t`tSendPacket(member, msg, len);" "tbInBroadcastLock = true; // [test]`r`n`t`t`tfor (const uint64_t member : room.Members)`r`n`t`t`t{`r`n`t`t`t`tSendPacket(member, msg, len);"
$t = Rep $t "deliver = static_cast<int64_t>(room.Members.size());" "tbInBroadcastLock = false; // [test]`r`n`t`t`tdeliver = static_cast<int64_t>(room.Members.size());"
[IO.File]::WriteAllText($p, $t, $bom)

$p = "$wt\networkLibProject\server.cfg"
$t = [IO.File]::ReadAllText($p)
$t = Rep $t 'chatRoomCnt = 0;' 'chatRoomCnt = 16;'
[IO.File]::WriteAllText($p, $t, $noBom)

$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
Set-Location $wt
foreach ($sln in 'networkLibProject\networkLibProject.sln', 'EchoClient\EchoClient.sln', 'LoadClient\LoadClient.sln') {
    & $msbuild $sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo | Select-String -Pattern 'error |warning C|networkLibProject.exe|LoadClient.exe' | ForEach-Object { $_.Line }
}
git diff --stat
"marker in exe: " + (Select-String -Path networkLibProject\x64\Release\networkLibProject.exe -Pattern 'deadlock-test' -Quiet)
"counter in dll: " + (Select-String -Path networkLibProject\x64\Release\NetworkLib.dll -Pattern 'defer-test' -Quiet)
