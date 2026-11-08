$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$gcc = (Get-Command gcc).Source
$tests = @("ppu_sprite_limit_test", "ppu_world_mirror_test", "ppu_elastic_band_test", "ppu_view_anchor_test", "ppu_mode7_hd_test", "ppu_mode7_reconstruct_test", "ppu_raster_journal_test", "ppu_hires_mode_test", "ppu_obj_priority_test", "ppu_brightness_test")
$failed = @()

foreach ($test in $tests) {
    $out = Join-Path $root "build\$test.exe"
    $args = @(
        "-std=c11", "-Wall", "-Wextra", "-O1",
        "-DSNESRECOMP_REVERSE_DEBUG=0",
        "-I$root\runner\src", "-I$root\runner\src\snes",
        "$root\tests\ppu\$test.c",
        "$root\runner\src\snes\ppu.c",
        "$root\runner\src\snes\hd_jobs.c",
        "$root\runner\src\snes\ppu_legacy.c",
        "-o", $out
    )
    if ($test -eq "ppu_mode7_reconstruct_test") {
        $args = $args | Where-Object { $_ -ne "$root\runner\src\snes\ppu.c" -and $_ -ne "$root\runner\src\snes\ppu_legacy.c" }
    }

    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
    $ErrorActionPreference = "Continue" # Native compiler warnings are stderr, not PowerShell failures.
    & $gcc @args
    $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0) { throw "gcc failed for $test" }
    if (-not (Test-Path -LiteralPath $out)) {
        throw "gcc did not produce $test.exe"
    }
    & $out
    if ($LASTEXITCODE -ne 0) { $failed += $test }
}

if ($failed.Count -gt 0) { throw ("PPU tests failed: " + ($failed -join ", ")) }
