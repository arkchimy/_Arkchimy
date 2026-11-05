$sp = $PSScriptRoot
# 파일을 다시 쓰면 BOM이 빠지므로 붙인다
$f = Join-Path $sp 'remeasure.ps1'
[IO.File]::WriteAllText($f, [IO.File]::ReadAllText($f), (New-Object Text.UTF8Encoding($true)))
# 1) skip82 전체(같은 클라로 두 크기) 3회 2) castfill 1회 3) now 중 대형 에코만 3회
& $f -Tag fix_skip82 -Stages skip82 -Rounds 3 -RunSec 15 2>&1 | Select-Object -Last 1
& $f -Tag fix_cast -Stages castfill -Rounds 1 -RunSec 15 2>&1 | Select-Object -Last 1
& $f -Tag fix_now -Stages nowbig -Rounds 3 -RunSec 15 2>&1 | Select-Object -Last 1
