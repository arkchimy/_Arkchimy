# Build each comparison commit with /O2 in its own worktree. The main working tree is not touched.
$repo = 'D:\_Arkchimy'
$wtRoot = Join-Path $PSScriptRoot 'wt'
New-Item -ItemType Directory -Force $wtRoot | Out-Null
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
$commits = '09a49f6', '2a91e23', '3a2ef87', '7de336d', '6754260', '649aa97', '3fed5a1'
Set-Location $repo
foreach ($c in $commits) {
    $wt = Join-Path $wtRoot $c
    if (-not (Test-Path $wt)) {
        git worktree add --detach $wt $c 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "worktree failed: $c" }
    }
    # same change as commit 8a57b0e: drop the debugging /Od line, keep the file's BOM and line endings
    $removed = 0
    foreach ($p in @("$wt\networkLibProject\networkLibProject.vcxproj", "$wt\_lib\ParserLib\ParserLib.vcxproj")) {
        $bytes = [IO.File]::ReadAllBytes($p)
        $t = [Text.Encoding]::UTF8.GetString($bytes)
        $n = [regex]::Replace($t, '[ \t]*<Optimization>Disabled</Optimization>\r?\n', '')
        if ($n -cne $t) {
            $removed++
            [IO.File]::WriteAllBytes($p, [Text.Encoding]::UTF8.GetBytes($n))
        }
    }
    $line = "$c optRemoved=$removed"
    foreach ($s in @('networkLibProject\networkLibProject.sln', 'EchoClient\EchoClient.sln', 'LoadClient\LoadClient.sln')) {
        $out = & $msbuild "$wt\$s" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo 2>&1
        $errs = @($out | Where-Object { $_ -match ': error ' })
        $warn = @($out | Where-Object { $_ -match ': warning ' -and $_ -notmatch 'MSB8029' })
        $line += " | $(Split-Path -Leaf $s) exit=$LASTEXITCODE err=$($errs.Count) warn=$($warn.Count)"
        $errs | Select-Object -First 3 | ForEach-Object { "   $_" }
    }
    $line
    $exe = "$wt\networkLibProject\x64\Release\networkLibProject.exe"
    $sw = @()
    Get-ChildItem "$wt\networkLibProject" -Recurse -Filter 'CL.command.1.tlog' | Where-Object { $_.FullName -match 'Release' } | ForEach-Object {
        $tl = [IO.File]::ReadAllText($_.FullName, [Text.Encoding]::Unicode)
        $sw += [regex]::Matches($tl, '(?<=\s)/(Od|O2|GL|MT|MD)(?=\s)') | ForEach-Object { $_.Value }
    }
    "   exe=" + (Test-Path $exe) + " switches=" + (($sw | Sort-Object -Unique) -join ' ') + " loadclient=" + (Test-Path "$wt\LoadClient\x64\Release\LoadClient.exe") + " echoclient=" + (Test-Path "$wt\EchoClient\x64\Release\EchoClient.exe")
}
git status --short
"--- worktrees"
git worktree list
