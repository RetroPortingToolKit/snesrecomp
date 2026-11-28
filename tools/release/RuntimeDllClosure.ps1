<#
Runtime DLL closure staging + verification for Windows release packages.

Why this exists
---------------
Every game's make_release.ps1 used to stage a HARDCODED list of MinGW runtime
DLLs (libgcc_s_seh-1, libstdc++-6, libwinpthread-1) next to the exe and SDL
backend. That list is a snapshot of what the import graph happened to look like
on the day it was written. When the build machine resolves a *different* SDL
build - e.g. SDL3 3.4.14, which links libiconv-2.dll, where 3.4.12 did not -
the new transitive dependency is silently omitted from the zip.

The shipped exe then loads fine on the developer's machine (MSYS2 / Git for
Windows put libiconv-2.dll on PATH) and fails on a player's machine:

  * dependency absent entirely  -> 0xC0000135 STATUS_DLL_NOT_FOUND
  * a 32-bit copy found on PATH -> 0xC000007B STATUS_INVALID_IMAGE_FORMAT
    ("The application was unable to start correctly")

The second case is the nastier one: the player has *some* unrelated 32-bit
libiconv-2.dll on PATH (32-bit Git for Windows, GTK, PHP, GIMP...), the loader
happily finds it, and the bitness mismatch aborts the process before main().
Nothing in the package is wrong-looking; the package is simply incomplete.

The fix is to stop enumerating dependencies by hand. This module walks the
actual PE import graph of the staged binaries, resolves every non-OS import
from the toolchain's bin directories, copies the closure into the stage, and
then re-verifies the stage is self-contained and uniformly 64-bit.

Usage from a game's make_release.ps1, replacing the hardcoded copy block:

  . (Join-Path $root 'snesrecomp\tools\release\RuntimeDllClosure.ps1')

  Copy-RuntimeDllClosure -StageDir $stage -SearchDirs @($build, $RuntimeBinDir)
  Assert-RuntimeDllClosure -StageDir $stage

Copy-RuntimeDllClosure seeds from every .exe/.dll already in the stage, so the
game-specific SDL copy (which picks build dir over RuntimeBinDir) still happens
in the caller; everything reachable from it is then pulled in automatically.
#>

# Deliberately NOT setting StrictMode here: this file is dot-sourced into each
# game's make_release.ps1, and a strict-mode change would leak into the caller.

# ---------------------------------------------------------------------------
# Minimal PE reader. Pure .NET so this works on any release host without
# needing objdump/dumpbin on PATH.
# ---------------------------------------------------------------------------

function Get-PeInfo {
  <#
    .SYNOPSIS
    Returns @{ Machine = 'x64'|'x86'|'arm64'|'0x...'; Imports = @('KERNEL32.dll', ...) }
    for a PE file, or $null if the file is not a PE image.
  #>
  param([Parameter(Mandatory = $true)][string]$Path)

  $bytes = [IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 0x40) { return $null }
  if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $null }   # 'MZ'

  $peOff = [BitConverter]::ToInt32($bytes, 0x3C)
  if ($peOff -le 0 -or ($peOff + 24) -ge $bytes.Length) { return $null }
  if ($bytes[$peOff] -ne 0x50 -or $bytes[$peOff + 1] -ne 0x45) { return $null }  # 'PE'

  $machineId = [BitConverter]::ToUInt16($bytes, $peOff + 4)
  $machine = switch ($machineId) {
    0x014C { 'x86' }
    0x8664 { 'x64' }
    0xAA64 { 'arm64' }
    default { ('0x{0:X4}' -f $machineId) }
  }

  $numSections = [BitConverter]::ToUInt16($bytes, $peOff + 6)
  $optSize = [BitConverter]::ToUInt16($bytes, $peOff + 20)
  $optOff = $peOff + 24
  $optMagic = [BitConverter]::ToUInt16($bytes, $optOff)
  # PE32+ (0x20B) has 16 extra bytes of 64-bit fields before the data
  # directories; PE32 (0x10B) does not.
  $dirOff = $optOff + $(if ($optMagic -eq 0x20B) { 112 } else { 96 })

  $info = @{ Machine = $machine; Imports = @() }
  if (($dirOff + 16) -gt $bytes.Length) { return $info }
  $importRva = [BitConverter]::ToUInt32($bytes, $dirOff + 8)   # directory[1] = import
  if ($importRva -eq 0) { return $info }

  # Section table follows the optional header.
  $sections = @()
  $secOff = $optOff + $optSize
  for ($i = 0; $i -lt $numSections; $i++) {
    $s = $secOff + (40 * $i)
    if (($s + 40) -gt $bytes.Length) { break }
    $sections += [pscustomobject]@{
      VirtualSize    = [BitConverter]::ToUInt32($bytes, $s + 8)
      VirtualAddress = [BitConverter]::ToUInt32($bytes, $s + 12)
      RawSize        = [BitConverter]::ToUInt32($bytes, $s + 16)
      RawPointer     = [BitConverter]::ToUInt32($bytes, $s + 20)
    }
  }

  # RVA -> file offset. Span the larger of virtual/raw size: a section whose
  # VirtualSize is smaller than SizeOfRawData (common for .idata folded into
  # .rdata) would otherwise reject valid RVAs.
  function Convert-RvaToOffset([uint32]$rva) {
    foreach ($s in $sections) {
      $span = [Math]::Max($s.VirtualSize, $s.RawSize)
      if ($rva -ge $s.VirtualAddress -and $rva -lt ($s.VirtualAddress + $span)) {
        $off = $s.RawPointer + ($rva - $s.VirtualAddress)
        if ($off -lt $bytes.Length) { return [int]$off }
        return -1
      }
    }
    return -1
  }

  function Read-CString([int]$offset) {
    if ($offset -lt 0 -or $offset -ge $bytes.Length) { return $null }
    $end = $offset
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
    return [Text.Encoding]::ASCII.GetString($bytes, $offset, $end - $offset)
  }

  $descOff = Convert-RvaToOffset $importRva
  if ($descOff -lt 0) { return $info }

  # IMAGE_IMPORT_DESCRIPTOR is 20 bytes; Name (the DLL string RVA) is at +12.
  # The array is terminated by an all-zero descriptor.
  $names = New-Object Collections.Generic.List[string]
  for ($i = 0; ; $i++) {
    $d = $descOff + (20 * $i)
    if (($d + 20) -gt $bytes.Length) { break }
    $nameRva = [BitConverter]::ToUInt32($bytes, $d + 12)
    if ($nameRva -eq 0) { break }
    $name = Read-CString (Convert-RvaToOffset $nameRva)
    if ([string]::IsNullOrWhiteSpace($name)) { break }
    $names.Add($name)
    if ($i -gt 4096) { break }   # malformed table guard
  }

  $info.Imports = $names.ToArray()
  return $info
}

# ---------------------------------------------------------------------------
# OS-provided classification
# ---------------------------------------------------------------------------

function Test-SystemDll {
  <#
    .SYNOPSIS
    True when an import is provided by Windows itself and must NOT be shipped.

    An import counts as OS-provided when it resolves under %SystemRoot%\System32
    (or is an api-ms-win-* / ext-ms-* API set). Everything else - libiconv-2.dll,
    libgcc_s_seh-1.dll, SDL3.dll, zlib1.dll - is ours to ship.
  #>
  param([Parameter(Mandatory = $true)][string]$Name)

  $lower = $Name.ToLowerInvariant()
  if ($lower.StartsWith('api-ms-win-') -or $lower.StartsWith('ext-ms-')) { return $true }

  $sys32 = Join-Path $env:SystemRoot 'System32'
  return (Test-Path -LiteralPath (Join-Path $sys32 $Name) -PathType Leaf)
}

# ---------------------------------------------------------------------------
# Stage enumeration
# ---------------------------------------------------------------------------

function Get-StagedPeFile {
  <#
    .SYNOPSIS
    Every .exe/.dll under the stage, recursively.

    .DESCRIPTION
    Extension filtering is done in the pipeline rather than with
    Get-ChildItem -Include: under PowerShell 5.1, -Include is silently ignored
    when the directory is supplied via -LiteralPath, which would make both the
    closure walk and the verification pass sweep non-PE payload (assets, .lua,
    .toml) and report it as corrupt.
  #>
  param([Parameter(Mandatory = $true)][string]$StageDir)

  Get-ChildItem -LiteralPath $StageDir -File -Recurse |
    Where-Object { $_.Extension -eq '.exe' -or $_.Extension -eq '.dll' }
}

# ---------------------------------------------------------------------------
# Closure staging
# ---------------------------------------------------------------------------

function Copy-RuntimeDllClosure {
  <#
    .SYNOPSIS
    Walk the import graph of every PE already in $StageDir and copy the full
    transitive set of non-OS dependencies in from $SearchDirs.

    .DESCRIPTION
    Throws when a dependency cannot be found in any search directory - an
    incomplete package must fail the build, not ship. Returns the list of DLL
    file names that were added, so the caller can log/diff them.
  #>
  param(
    [Parameter(Mandatory = $true)][string]$StageDir,
    [Parameter(Mandatory = $true)][string[]]$SearchDirs
  )

  $dirs = @($SearchDirs | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
  if ($dirs.Count -eq 0) {
    throw "Copy-RuntimeDllClosure: no usable search directory in: $($SearchDirs -join ', ')"
  }

  $seen = New-Object Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
  $queue = New-Object Collections.Generic.Queue[string]
  $added = New-Object Collections.Generic.List[string]

  # NB: Get-ChildItem -Include is silently ignored when the path is passed via
  # -LiteralPath (PowerShell 5.1), so filter on the extension explicitly.
  foreach ($f in Get-StagedPeFile -StageDir $StageDir) {
    [void]$seen.Add($f.Name)
    $queue.Enqueue($f.FullName)
  }
  if ($queue.Count -eq 0) {
    throw "Copy-RuntimeDllClosure: no .exe/.dll staged under $StageDir"
  }

  while ($queue.Count -gt 0) {
    $path = $queue.Dequeue()
    $pe = Get-PeInfo -Path $path
    if (-not $pe) { continue }

    foreach ($dep in $pe.Imports) {
      if ($seen.Contains($dep)) { continue }
      if (Test-SystemDll -Name $dep) { [void]$seen.Add($dep); continue }

      $source = $null
      foreach ($d in $dirs) {
        $candidate = Join-Path $d $dep
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $source = $candidate; break }
      }
      if (-not $source) {
        throw ("Unresolved runtime dependency '$dep' required by " +
          "$([IO.Path]::GetFileName($path)). Searched: $($dirs -join ', '). " +
          'Shipping without it produces 0xC0000135 on a clean machine, or ' +
          '0xC000007B if the player has a 32-bit copy on PATH.')
      }

      Copy-Item -LiteralPath $source -Destination $StageDir -Force
      [void]$seen.Add($dep)
      $added.Add($dep)
      $queue.Enqueue((Join-Path $StageDir $dep))
    }
  }

  return $added.ToArray()
}

function Assert-RuntimeDllClosure {
  <#
    .SYNOPSIS
    Verify the staged package is self-contained and uniformly one architecture.

    .DESCRIPTION
    Re-reads the staged tree rather than trusting the copy pass, so a stale
    stage directory or a hand-edited zip layout is caught too. Two invariants:

      1. every non-OS import of every staged PE is present in the stage
      2. every staged PE has the same machine type

    Invariant 2 is what a hand-maintained DLL list can never check: a 32-bit
    DLL dropped into an x64 package is precisely the 0xC000007B failure.
  #>
  param(
    [Parameter(Mandatory = $true)][string]$StageDir,
    [string]$ExpectedMachine = 'x64'
  )

  $staged = @(Get-StagedPeFile -StageDir $StageDir)
  if ($staged.Count -eq 0) {
    throw "Assert-RuntimeDllClosure: no .exe/.dll staged under $StageDir"
  }

  $present = New-Object Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
  foreach ($f in $staged) { [void]$present.Add($f.Name) }

  $problems = New-Object Collections.Generic.List[string]
  foreach ($f in $staged) {
    $pe = Get-PeInfo -Path $f.FullName
    if (-not $pe) { $problems.Add("$($f.Name): not a valid PE image"); continue }

    if ($pe.Machine -ne $ExpectedMachine) {
      $problems.Add("$($f.Name): machine $($pe.Machine), expected $ExpectedMachine")
    }
    foreach ($dep in $pe.Imports) {
      if ($present.Contains($dep)) { continue }
      if (Test-SystemDll -Name $dep) { continue }
      $problems.Add("$($f.Name): imports '$dep', which is neither staged nor OS-provided")
    }
  }

  if ($problems.Count -gt 0) {
    throw ("Release stage is not self-contained:`n  " + ($problems -join "`n  "))
  }

  return $staged.Count
}
