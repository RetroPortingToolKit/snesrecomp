$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\program_module_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\program_module\program_module_test.c",
    "$root\runner\src\program_module.c",
    "$root\runner\src\sha256.c",
    "-o", $out
)
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
& $gcc @args
if ($LASTEXITCODE -ne 0) { throw "program module test build failed" }
& $out
if ($LASTEXITCODE -ne 0) { throw "program module tests failed" }
