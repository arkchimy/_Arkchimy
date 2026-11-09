param([ValidateSet('base','coarse','fine')][string]$Variant, [string]$Wt)
# Applies measuring scopes on top of HEAD in the temporary worktree, then builds. Never committed.
$repo = 'D:\_Arkchimy'
$wt = $Wt
$bom = New-Object Text.UTF8Encoding($true)
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
function Rep([string]$text, [string]$old, [string]$new) {
    $n = ([regex]::Matches($text, [regex]::Escape($old))).Count
    if ($n -ne 1) { throw "anchor count $n : $old" }
    return $text.Replace($old, $new)
}
$t = [IO.File]::ReadAllText("$repo\_lib\NetworkLib\NetworkLib.cpp")
if ($Variant -ne 'base') {
    $t = Rep $t 'OnRecv(session.mID, payload, header.Len);' '{ ProfileScope onRecvScope("OnRecv"); OnRecv(session.mID, payload, header.Len); }'
    $t = Rep $t "handleCompletion(*static_cast<MyOverlapped*>(overlapped), reinterpret_cast<Session*>(key), bOk, transferred);`r`n`t`tflushSends();" "handleCompletion(*static_cast<MyOverlapped*>(overlapped), reinterpret_cast<Session*>(key), bOk, transferred);`r`n`t`t{ ProfileScope flushScope(`"Flush`"); flushSends(); }"
    $t = Rep $t "bIoOk, entry.dwNumberOfBytesTransferred);`r`n`t`t}`r`n`t`tflushSends();" "bIoOk, entry.dwNumberOfBytesTransferred);`r`n`t`t}`r`n`t`t{ ProfileScope flushScope(`"Flush`"); flushSends(); }"
}
if ($Variant -eq 'fine') {
    $t = Rep $t "Session* const session = findSession(id);`r`n`tif (session == nullptr)`r`n`t{`r`n`t`treturn false; // 이미 해제됨" "ProfileScope sendPacketScope(`"SendPacket`");`r`n`tSession* const session = findSession(id);`r`n`tif (session == nullptr)`r`n`t{`r`n`t`treturn false; // 이미 해제됨"
    $t = Rep $t "std::lock_guard<std::mutex> sendLock(session.mSendLock);`r`n`t`t// 헤더와 페이로드" "ProfileScope pushScope(`"LockPush`");`r`n`t`tstd::lock_guard<std::mutex> sendLock(session.mSendLock);`r`n`t`t// 헤더와 페이로드"
}
[IO.File]::WriteAllText("$wt\_lib\NetworkLib\NetworkLib.cpp", $t, $bom)
foreach ($h in "NetworkLib.h","Session.h") { Copy-Item "$repo\_lib\NetworkLib\$h" "$wt\_lib\NetworkLib\$h" -Force }
Set-Location $wt
foreach ($sln in 'networkLibProject\networkLibProject.sln', 'LoadClient\LoadClient.sln') {
    & $msbuild $sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo | Select-String -Pattern 'error |warning C' | ForEach-Object { $_.Line }
}
New-Item -ItemType Directory -Force "$wt\prof_bin\$Variant" | Out-Null
Copy-Item "$wt\networkLibProject\x64\Release\networkLibProject.exe", "$wt\networkLibProject\x64\Release\NetworkLib.dll" "$wt\prof_bin\$Variant\" -Force
"built $Variant : " + (Get-Item "$wt\prof_bin\$Variant\NetworkLib.dll").LastWriteTime
