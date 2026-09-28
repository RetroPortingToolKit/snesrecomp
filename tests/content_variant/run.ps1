$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$outDir = Join-Path $root "build"
$out = Join-Path $outDir "content_variant_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-I$root\runner\src", "-I$root\runner\src\snes", "-I$root\third_party",
    "-DSNESRECOMP_ENABLE_MODS=0",
    "$root\tests\content_variant\content_variant_test.c",
    "$root\runner\src\content_variant.c",
    "$root\runner\src\variant_selector.c",
    "$root\runner\src\program_module.c",
    "$root\runner\src\rom_patch.c",
    "$root\runner\src\crc32.c",
    "$root\runner\src\sha256.c",
    "$root\runner\src\snes_overlay_draw.c",
    "-o", $out
)
New-Item -ItemType Directory -Force $outDir | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
& $gcc @args
if ($LASTEXITCODE -ne 0) { throw "content variant test build failed" }
Push-Location $outDir
try { & $out; if ($LASTEXITCODE -ne 0) { throw "content variant tests failed" } }
finally { Pop-Location }
