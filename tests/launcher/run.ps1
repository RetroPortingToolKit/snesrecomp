$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\launcher_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
    "-I$root\runner\src",
    "$root\tests\launcher\launcher_test.c",
    "$root\runner\src\launcher.c",
    "$root\runner\src\launcher_cache.c",
    "$root\runner\src\launcher_picker.c",
    "$root\runner\src\rom_image_verify.c",
    "$root\runner\src\rom_identity.c",
    "$root\runner\src\host_paths.c",
    "$root\runner\src\crc32.c",
    "$root\runner\src\sha1.c",
    "$root\runner\src\sha256.c",
    "-lcomdlg32",
    "-o", $out
)

New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath $gcc -ArgumentList $args `
    -PassThru -NoNewWindow -Wait
if ($proc.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $out)) {
    throw "gcc failed to build launcher_test"
}
& $out
if ($LASTEXITCODE -ne 0) { throw "launcher_test failed" }
