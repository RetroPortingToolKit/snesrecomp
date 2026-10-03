$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\rom_patch_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-I$root\runner\src",
    "$root\tests\rom_patch\rom_patch_test.c",
    "$root\runner\src\rom_patch.c",
    "$root\runner\src\crc32.c",
    "-o", $out
)
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
& $gcc @args
if ($LASTEXITCODE -ne 0) { throw "ROM patch test build failed" }
& $out
if ($LASTEXITCODE -ne 0) { throw "ROM patch tests failed" }
