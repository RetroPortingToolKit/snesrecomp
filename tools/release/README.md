# Shared runtime release tools

`build_linux.sh --project-root <game>` configures, builds and stages an
AppImage. A game's `tools/build-linux.sh` is a thin wrapper; its
`tools/linux-release.sh` owns names, target, required mod manifests, payload
paths and optional title hooks. `--build` and `--out` select external build and
output directories. Parallel builds default to two jobs and honor
`CMAKE_BUILD_PARALLEL_LEVEL` or `--jobs`.

The driver owns versioned packaging-tool pins, launcher assets, state anchoring,
mod seeding, controller hints and portable-state checks. Profiles can declare
`configure_game`, `stage_game_payload`, `check_game_payload` and
`test_game_package` functions. A title-specific `tools/apprun-extra.sh` is copied
into AppRun after the writable state directory is resolved. It must preserve
user settings and data. Qualified prebuilt ELF reuse retains source, dependency,
native-provider and input-corpus identity checks.

PowerShell packagers dot-source `RuntimeRelease.ps1`. Titles stage their
payload and notices, then call `Copy-RuntimeDllClosure`,
`Assert-RuntimeDllClosure` and `New-RuntimeReleaseArchive`. The archive writer
normalizes paths and verifies every extracted entry's size and SHA-256 against
the stage. Python packagers use `runtime_dependencies.bundle_windows_runtime`
to invoke the same pure .NET PE reader with explicit search directories.
The cross-platform `tools/ci/bundle_runtime_libs.sh` remains the shared path
for portable native archives, including macOS and Linux.
