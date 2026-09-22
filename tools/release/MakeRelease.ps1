<#
Package a completed Windows release build for any snesrecomp title.

Every port used to carry its own ~220-line copy of this pipeline. They agreed
on all of it except a handful of names and payload items, and that duplication
was not free: one SDL bump that added a transitive `libiconv-2.dll` dependency
had to be fixed in ten repositories, two of them were still shipping the old
hardcoded DLL list weeks later, and six titles had no packaging script at all
and therefore no dependency guard of any kind.

A title now declares WHAT it ships. This module owns HOW a release is staged,
verified and archived:

  * the executable is stamped with the version it is being packaged as
  * launcher assets, the mod catalog and per-title payload are staged
  * the SDL backend plus the full transitive PE import closure is staged, and
    re-verified from the stage (RuntimeDllClosure.ps1)
  * the mod catalog actually arrives, when the title has one
  * nothing on a per-title forbid list (owner ROMs, extracted assets) leaks in
  * the zip is written with portable '/' entry names and read back to prove it

Usage from a title's tools/make_release.ps1:

    $root = Split-Path -Parent $PSScriptRoot
    . (Join-Path $root 'snesrecomp\tools\release\MakeRelease.ps1')
    Invoke-SnesRecompRelease -Root $root -Version $Version `
      -ExeName 'MyGameSNESRecomp.exe' -StageBase 'MyGameSNESRecomp' `
      -BuildDir $BuildDir -RuntimeBinDir $RuntimeBinDir `
      -Payload @(
        @{ Source = 'root';  Path = 'LICENSE' }
        @{ Source = 'root';  Path = 'docs/*.md'; Into = 'docs' }
        @{ Source = 'build'; Path = 'lua'; WhenCMakeOption = 'SNESRECOMP_ENABLE_LUA'
           Require = @('README.md') }
      ) `
      -ConfigOverrides @{ Widescreen = '0' }

Payload item keys (all optional except Path):
  Source           'root' (default) or 'build' — which tree Path is relative to
  Path             file, directory, or a leaf wildcard like 'docs/*.md'
  Into             destination directory inside the stage; default stage root
  Optional         $true to skip silently when absent; default $false
  Require          names that must exist under a staged directory
  WhenCMakeOption  stage only if this CMake option is ON in the build's cache

This module does NOT build. Build first, then package.
#>

# NOTE: deliberately no `Set-StrictMode` here. This file is dot-sourced into
# a caller's scope, and a strict-mode setting made at module scope leaks into
# that caller and changes the behaviour of code this module does not own.

. (Join-Path $PSScriptRoot 'RuntimeDllClosure.ps1')


function Test-CMakeOptionEnabled {
    <#
    True when <Option>:BOOL=ON appears in the build directory's CMake cache.
    A missing cache is not an error: it means "not enabled", which is what a
    conditional payload item should do rather than failing the release.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$Option
    )
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) { return $false }
    return [bool](Select-String -LiteralPath $cache `
        -Pattern ("^" + [regex]::Escape($Option) + ":BOOL=ON$") -Quiet)
}


function Copy-ReleasePayloadItem {
    <#
    Stage one payload item. Returns the destination paths it produced.
    #>
    param(
        [Parameter(Mandatory = $true)][hashtable]$Item,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$StageDir
    )

    if (-not $Item.ContainsKey('Path')) {
        throw 'Release payload item is missing its Path key.'
    }
    $source = if ($Item.ContainsKey('Source')) { $Item.Source } else { 'root' }
    if ($source -notin @('root', 'build')) {
        throw "Release payload item Source must be 'root' or 'build', got '$source'."
    }
    $base = if ($source -eq 'build') { $BuildDir } else { $Root }
    $optional = [bool]$Item.Optional

    # Normalise to backslashes so Join-Path and -LiteralPath agree, and keep
    # the wildcard (if any) confined to the leaf.
    $relative = ($Item.Path -replace '/', '\')
    $full = Join-Path $base $relative
    $into = if ($Item.ContainsKey('Into') -and $Item.Into) {
        Join-Path $StageDir ($Item.Into -replace '/', '\')
    } else { $StageDir }

    $leaf = Split-Path -Leaf $relative
    $staged = @()

    if ($leaf.Contains('*') -or $leaf.Contains('?')) {
        $parent = Split-Path -Parent $full
        if (-not (Test-Path -LiteralPath $parent)) {
            if ($optional) { return @() }
            throw "Release payload directory missing: $parent"
        }
        # PS 5.1 silently ignores -Include when -LiteralPath is used, so the
        # match is done with -Filter on the leaf pattern instead.
        $matches = @(Get-ChildItem -LiteralPath $parent -Filter $leaf -File)
        if ($matches.Count -eq 0) {
            if ($optional) { return @() }
            throw "Release payload matched nothing: $full"
        }
        if (-not (Test-Path -LiteralPath $into)) {
            New-Item -ItemType Directory -Path $into -Force | Out-Null
        }
        foreach ($match in $matches) {
            Copy-Item -LiteralPath $match.FullName -Destination $into
            $staged += (Join-Path $into $match.Name)
        }
        return $staged
    }

    if (-not (Test-Path -LiteralPath $full)) {
        if ($optional) { return @() }
        throw "Release payload missing: $full"
    }

    if (-not (Test-Path -LiteralPath $into)) {
        New-Item -ItemType Directory -Path $into -Force | Out-Null
    }

    $isDirectory = (Get-Item -LiteralPath $full).PSIsContainer
    if ($isDirectory) {
        Copy-Item -LiteralPath $full -Destination $into -Recurse
        $destination = Join-Path $into $leaf
        foreach ($name in @($Item.Require)) {
            if (-not $name) { continue }
            $member = Join-Path $destination ($name -replace '/', '\')
            if (-not (Test-Path -LiteralPath $member)) {
                throw "Release payload '$relative' is missing required member: $name"
            }
        }
        $staged += $destination
    } else {
        Copy-Item -LiteralPath $full -Destination $into
        $staged += (Join-Path $into $leaf)
    }
    return $staged
}


function Assert-ReleaseModCatalog {
    <#
    A title whose repository holds mod packages must ship them.

    This exists because the failure is silent and has shipped twice: the mod
    page renders empty, the executable runs, and nothing in the build or the
    packaging step complains. Zelda shipped that way because no target declared
    its catalog; Super Mario World shipped that way because the release script
    looked for `mods/packages` while the engine stages the canonical
    `mods/preloaded/packages`. Checking the source of truth against the stage
    catches both shapes, and any future third.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$StageDir
    )
    $sourcePackages = Join-Path $Root 'mods\preloaded\packages'
    if (-not (Test-Path -LiteralPath $sourcePackages)) { return }
    $expected = @(Get-ChildItem -LiteralPath $sourcePackages -Directory)
    if ($expected.Count -eq 0) { return }

    $stagedPackages = Join-Path $StageDir 'mods\preloaded\packages'
    if (-not (Test-Path -LiteralPath $stagedPackages)) {
        throw ("This title has $($expected.Count) mod package(s) under " +
            "mods\preloaded\packages but the release stage has no " +
            "mods\preloaded\packages at all, so the Mods page would ship " +
            'empty. The build stages the catalog beside the executable ' +
            '(snesrecomp_target_mod_catalog); package the build output, not ' +
            'the source tree.')
    }
    $missing = @($expected | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $stagedPackages $_.Name))
    })
    if ($missing.Count -ne 0) {
        throw ('Release stage is missing mod package(s): ' +
            (($missing | ForEach-Object Name) -join ', '))
    }
}


function Assert-ReleaseStageClean {
    <#
    Reject anything a title forbids from its package — owner ROMs, assets
    extracted from one, caches. Path patterns are matched against the
    stage-relative '/'-separated path; magic entries map a leading byte
    signature to the description used in the error.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$StageDir,
        [string[]]$ForbidPathPattern = @(),
        [hashtable]$ForbidMagic = @{}
    )
    if ($ForbidPathPattern.Count -eq 0 -and $ForbidMagic.Count -eq 0) { return }

    $prefix = [IO.Path]::GetFullPath($StageDir).TrimEnd('\') + '\'
    $longest = 0
    foreach ($signature in $ForbidMagic.Keys) {
        if ($signature.Length -gt $longest) { $longest = $signature.Length }
    }

    foreach ($file in @(Get-ChildItem -LiteralPath $StageDir -File -Recurse)) {
        $relative = [IO.Path]::GetFullPath($file.FullName).Substring(
            $prefix.Length).Replace('\', '/')
        foreach ($pattern in $ForbidPathPattern) {
            if ($relative -match $pattern) {
                throw "Forbidden content in release stage: $relative (matched $pattern)"
            }
        }
        if ($longest -le 0) { continue }
        $header = New-Object byte[] $longest
        $stream = [IO.File]::OpenRead($file.FullName)
        try {
            $read = $stream.Read($header, 0, $longest)
        } finally {
            $stream.Dispose()
        }
        if ($read -le 0) { continue }
        $text = [Text.Encoding]::ASCII.GetString($header, 0, $read)
        foreach ($signature in $ForbidMagic.Keys) {
            if ($text.StartsWith($signature)) {
                throw ("Forbidden content in release stage: $relative is " +
                    "$($ForbidMagic[$signature])")
            }
        }
    }
}


function Write-PortableZip {
    <#
    Write the stage to a zip using '/' entry names, then read it back and
    reject any non-portable name.

    Compress-Archive preserves Windows backslashes, which POSIX extractors may
    treat as literal filename characters rather than directory separators. That
    is what stopped Linux / Steam Deck extractors from rebuilding the nested
    assets/ and mods/ hierarchies, so a Windows build under Proton could not
    find its own launcher assets or mod catalog.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$StageDir,
        [Parameter(Mandatory = $true)][string]$ZipPath
    )
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $stageFull = [IO.Path]::GetFullPath($StageDir)
    $prefix = $stageFull.TrimEnd('\') + '\'
    $zipFull = [IO.Path]::GetFullPath($ZipPath)
    $files = @(Get-ChildItem -LiteralPath $StageDir -File -Recurse |
        Sort-Object FullName)

    $archive = [IO.Compression.ZipFile]::Open(
        $zipFull, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in $files) {
            $fileFull = [IO.Path]::GetFullPath($file.FullName)
            if (-not $fileFull.StartsWith(
                    $prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to archive a file outside the release stage: $fileFull"
            }
            $entryName = $fileFull.Substring($prefix.Length).Replace('\', '/')
            if ($entryName.StartsWith('/') -or $entryName -match '(^|/)\.\.(/|$)') {
                throw "Unsafe ZIP entry name: $entryName"
            }
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $fileFull, $entryName,
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }

    $archive = [IO.Compression.ZipFile]::OpenRead($zipFull)
    try {
        $bad = @($archive.Entries | Where-Object {
            $_.FullName.Contains('\') -or
            $_.FullName.StartsWith('/') -or
            $_.FullName -match '(^|/)\.\.(/|$)'
        })
        if ($bad.Count -ne 0) {
            throw ('ZIP contains non-portable entry names: ' +
                (($bad | ForEach-Object FullName) -join ', '))
        }
        if ($archive.Entries.Count -ne $files.Count) {
            throw ("ZIP entry count mismatch: expected $($files.Count), " +
                "got $($archive.Entries.Count)")
        }
    } finally {
        $archive.Dispose()
    }
    return $files.Count
}


function Invoke-SnesRecompRelease {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$ExeName,
        [Parameter(Mandatory = $true)][string]$StageBase,
        [string]$BuildDir = 'build-recompui',
        [string]$RuntimeBinDir = 'C:\msys64\mingw64\bin',
        [ValidateSet('SDL3', 'SDL2')][string]$SdlBackend = 'SDL3',
        [hashtable[]]$Payload = @(),
        [hashtable]$ConfigOverrides = @{},
        [string[]]$ForbidPathPattern = @(),
        [hashtable]$ForbidMagic = @{},
        [switch]$AllowUnstampedExe
    )

    $ErrorActionPreference = 'Stop'

    $build = if ([IO.Path]::IsPathRooted($BuildDir)) {
        $BuildDir
    } else {
        Join-Path $Root $BuildDir
    }
    $exe = Join-Path $build $ExeName
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "Release executable missing: $exe"
    }

    # The executable must carry the version it is being packaged as. host_report
    # stamps crash breadcrumbs with SNESRECOMP_BUILD_VERSION, so a build
    # configured without it would ship reports that cannot be tied to this
    # release. PowerShell silently rewrites an unquoted
    # `-DSNESRECOMP_BUILD_VERSION=0.10.0` to `0`, which is exactly how that
    # happens in practice — always pass it quoted as
    # "-DSNESRECOMP_BUILD_VERSION:STRING=<version>".
    if (-not $AllowUnstampedExe) {
        $exeText = [Text.Encoding]::ASCII.GetString(
            [IO.File]::ReadAllBytes($exe))
        if (-not $exeText.Contains($Version)) {
            throw ("Release executable is not stamped with version " +
                "'$Version'. Reconfigure with " +
                "`"-DSNESRECOMP_BUILD_VERSION:STRING=$Version`" and rebuild " +
                'before packaging.')
        }
    }

    $out = Join-Path $Root 'release-stage'
    $stageName = "$StageBase-windows-x64-v$Version"
    $stage = Join-Path $out $stageName
    $zip = Join-Path $out "$stageName.zip"

    $outFull = [IO.Path]::GetFullPath($out).TrimEnd('\') + '\'
    $stageFull = [IO.Path]::GetFullPath($stage)
    $zipFull = [IO.Path]::GetFullPath($zip)
    if (-not $stageFull.StartsWith($outFull, [StringComparison]::OrdinalIgnoreCase) -or
        -not $zipFull.StartsWith($outFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to clean release paths outside release-stage.'
    }
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
    if (Test-Path -LiteralPath $zip) {
        Remove-Item -LiteralPath $zip -Force
    }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    Copy-Item -LiteralPath $exe -Destination $stage

    # What every title ships. `mods` is staged whole rather than reaching for a
    # known subdirectory: the canonical layout beside the executable is
    # mods/preloaded/packages (SNESRECOMP_MOD_CATALOG_DEST), and a release
    # script that hardcodes one level of that path silently ships no mods when
    # the layout moves. Assert-ReleaseModCatalog then proves it arrived.
    $standard = @(
        @{ Source = 'build'; Path = 'assets' }
        @{ Source = 'build'; Path = 'mods';         Optional = $true }
        @{ Source = 'build'; Path = 'keybinds.ini'; Optional = $true }
        @{ Source = 'root';  Path = 'README.md' }
    )
    foreach ($item in ($standard + $Payload)) {
        if ($item.ContainsKey('WhenCMakeOption') -and $item.WhenCMakeOption) {
            if (-not (Test-CMakeOptionEnabled -BuildDir $build `
                    -Option $item.WhenCMakeOption)) {
                continue
            }
        }
        Copy-ReleasePayloadItem -Item $item -Root $Root -BuildDir $build `
            -StageDir $stage | Out-Null
    }

    # config.ini ships with the title's release defaults regardless of the
    # working tree's values (a developer may have flipped one while testing);
    # the launcher persists the player's choice at runtime.
    $configSource = Join-Path $Root 'config.ini'
    if (Test-Path -LiteralPath $configSource) {
        $lines = Get-Content -LiteralPath $configSource
        foreach ($key in $ConfigOverrides.Keys) {
            $pattern = '^' + [regex]::Escape($key) + '\s*=.*$'
            $value = $ConfigOverrides[$key]
            $replacement = if ($value -eq '') { "$key =" } else { "$key = $value" }
            $lines = $lines -replace $pattern, $replacement
        }
        $lines | Out-File (Join-Path $stage 'config.ini') -Encoding ascii
    } elseif ($ConfigOverrides.Count -ne 0) {
        throw ("ConfigOverrides were supplied but this title has no " +
            "config.ini at $configSource")
    }

    # Runtime DLLs are NOT enumerated by hand. A hardcoded list is a snapshot
    # of the import graph on the day it was written and silently omits whatever
    # a later toolchain or SDL bump pulls in — SDL3 3.4.14 links
    # libiconv-2.dll where 3.4.12 did not. The gap is invisible on a developer
    # box (MSYS2 and Git for Windows both put the DLL on PATH) and fatal on a
    # player's machine: 0xC0000135 when it is simply absent, or 0xC000007B
    # when the loader binds an unrelated 32-bit copy from PATH.
    $sdlDll = "$SdlBackend.dll"
    $sdlSource = Join-Path $build $sdlDll
    if (-not (Test-Path -LiteralPath $sdlSource)) {
        $sdlSource = Join-Path $RuntimeBinDir $sdlDll
    }
    if (-not (Test-Path -LiteralPath $sdlSource)) {
        throw ("Required $SdlBackend runtime DLL missing from build or " +
            "runtime bin: $sdlDll")
    }
    Copy-Item -LiteralPath $sdlSource -Destination $stage

    $stagedDlls = Copy-RuntimeDllClosure -StageDir $stage `
        -SearchDirs @($build, $RuntimeBinDir)
    if ($stagedDlls.Count -gt 0) {
        Write-Host "Staged runtime dependency closure: $($stagedDlls -join ', ')"
    }
    # Re-read the stage rather than trusting the copy pass: every non-OS import
    # must resolve inside the package, and every staged PE must be x64.
    Assert-RuntimeDllClosure -StageDir $stage | Out-Null

    Assert-ReleaseModCatalog -Root $Root -StageDir $stage
    Assert-ReleaseStageClean -StageDir $stage `
        -ForbidPathPattern $ForbidPathPattern -ForbidMagic $ForbidMagic

    $count = Write-PortableZip -StageDir $stage -ZipPath $zip

    Write-Host "--- $stageName ($count files) ---"
    Get-ChildItem -LiteralPath $stage | Select-Object Name, Length | Out-Host
    Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Out-Host
    return $zip
}
