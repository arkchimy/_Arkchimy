@echo off
rem Build atomic_bench.exe (Release x64) into x64\ - run from any folder
set PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
if not exist x64 mkdir x64
rem /wd4324: padding from alignas(64) is intended (one counter per cache line)
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W4 /wd4324 /FAs /Fax64\atomic_bench.asm atomic_bench.cpp /Fo:x64\ /Fe:x64\atomic_bench.exe
