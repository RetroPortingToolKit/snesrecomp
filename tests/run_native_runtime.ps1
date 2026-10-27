param(
    [string]$Compiler = 'C:\msys64\mingw64\bin\gcc.exe',
    [string]$OutputDirectory = '',
    [switch]$Bench
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'build\native-runtime' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$common = @('-std=c11', '-O3', '-UNDEBUG', '-DSNESRECOMP_TRACE=0',
    '-ffunction-sections', '-fdata-sections', "-I$root\runner\src")
$audioSources = @("$root\tests\audio\apu_native_test.c",
    "$root\runner\src\snes\spc.c", "$root\runner\src\snes\dsp.c",
    "$root\runner\src\snes\dsp_shadow.c", "$root\runner\src\snes\audio_shadow.c",
    "$root\runner\src\audio_trace.c")
foreach ($native in @(0, 1)) {
    $out = Join-Path $OutputDirectory "apu-native-$native.exe"
    & $Compiler @common "-DSNESRECOMP_APU_NATIVE=$native" @audioSources '-Wl,--gc-sections' '-lm' '-o' $out
    if ($LASTEXITCODE -ne 0) { throw "APU build failed: $native" }
    & $out
    if ($LASTEXITCODE -ne 0) { throw "APU execution failed: $native" }
    if ($Bench -and $native) {
        & $out --bench
        if ($LASTEXITCODE -ne 0) { throw 'APU component benchmark failed' }
    }
}
$out = Join-Path $OutputDirectory 'ppu-native-raster.exe'
& $Compiler @common '-DSNESRECOMP_REVERSE_DEBUG=0' "-I$root\runner\src\snes" `
    "$root\tests\ppu\ppu_native_raster_test.c" "$root\runner\src\snes\ppu_legacy.c" '-o' $out
if ($LASTEXITCODE -ne 0) { throw 'PPU build failed' }
& $out
if ($LASTEXITCODE -ne 0) { throw 'PPU execution failed' }
foreach ($slices in @(1, 8)) {
    $out = Join-Path $OutputDirectory "crc32-$slices.exe"
    & $Compiler @common "-DSNESRECOMP_CRC32_SLICES=$slices" `
        "$root\tests\crc32_native_test.c" "$root\runner\src\crc32.c" '-o' $out
    if ($LASTEXITCODE -ne 0) { throw "CRC build failed: $slices" }
    & $out
    if ($LASTEXITCODE -ne 0) { throw "CRC execution failed: $slices" }
}
