param([string]$Compiler = 'C:\msys64\mingw64\bin\gcc.exe')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$output = Join-Path $repo 'build-tests/audio-delivery'
[void](New-Item -ItemType Directory -Path $output -Force)
$executable = Join-Path $output 'audio_delivery_test.exe'
Push-Location -LiteralPath $repo
try {
    & $Compiler -std=c11 -O2 -flto -fwhole-program -ffunction-sections -fdata-sections `
        -DSNESRECOMP_TRACE=0 -DSNESRECOMP_SDL3=1 -DSDL_MAIN_HANDLED `
        -Irunner/src/desktop -Irunner/src tests/audio_delivery_test.c `
        runner/src/snes/dsp.c runner/src/snes/dsp_clock.c runner/src/audio_trace.c '-Wl,--gc-sections' -lm -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'Audio delivery test compilation failed' }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw 'Audio delivery regression failed' }
} finally {
    Pop-Location
}
