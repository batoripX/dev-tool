$ErrorActionPreference = "Stop"
$LlvmRoot = Split-Path -Parent (Split-Path -Parent (Get-Command clang++).Source)
$StdCppm  = "$LlvmRoot\share\libc++\v1\std.cppm"
$Flags    = @('-std=c++23', '-O3', '-DNDEBUG', '-march=x86-64-v3', '-flto=thin', '-ffunction-sections', '-fdata-sections')

clang++ @Flags -Wno-reserved-module-identifier --precompile "$StdCppm" -o std.pcm
clang++ @Flags '-fmodule-file=std=std.pcm' --precompile lock.cppm -o dev.lock.pcm
clang++ @Flags '-fmodule-file=std=std.pcm' --precompile build.cppm -o dev.build.pcm
clang++ @Flags '-fmodule-file=std=std.pcm' --precompile command.cppm -o dev.command.pcm

clang++ @Flags -static -s `
    '-fmodule-file=std=std.pcm' `
    '-fmodule-file=dev.lock=dev.lock.pcm' `
    '-fmodule-file=dev.build=dev.build.pcm' `
    '-fmodule-file=dev.command=dev.command.pcm' `
    std.pcm lock.cppm build.cppm command.cppm main.cppm `
    '-Wl,--gc-sections' -lws2_32 -liphlpapi -lrstrtmgr `
    -o dev.exe

Remove-Item -Force *.pcm
