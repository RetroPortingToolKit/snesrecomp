# Shared release primitives. Titles select their payload, defaults and notices.
. (Join-Path $PSScriptRoot 'RuntimeDllClosure.ps1')

function New-RuntimeReleaseArchive {
  param(
    [Parameter(Mandatory = $true)][string]$StageDir,
    [Parameter(Mandatory = $true)][string]$ArchivePath
  )
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $stage = [IO.Path]::GetFullPath($StageDir).TrimEnd('\') + '\'
  $zip = [IO.Path]::GetFullPath($ArchivePath)
  if ($zip.StartsWith($stage, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The archive must be outside its input stage.'
  }
  $files = @(Get-ChildItem -LiteralPath $StageDir -File -Recurse | Sort-Object FullName)
  $entries = @{}
  foreach ($file in $files) {
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
      throw "Release payload cannot contain links: $($file.FullName)"
    }
    $full = [IO.Path]::GetFullPath($file.FullName)
    if (-not $full.StartsWith($stage, [StringComparison]::OrdinalIgnoreCase)) {
      throw "Release input is outside the stage: $full"
    }
    $name = $full.Substring($stage.Length).Replace('\', '/')
    if ($name.StartsWith('/') -or $name -match '(^|/)\.\.(/|$)' -or $entries.ContainsKey($name)) {
      throw "Unsafe or duplicate ZIP entry: $name"
    }
    $entries[$name] = $file
  }
  $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($name in @($entries.Keys | Sort-Object)) {
      [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $archive, $entries[$name].FullName, $name,
        [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
  } finally { $archive.Dispose() }
  $archive = [IO.Compression.ZipFile]::OpenRead($zip)
  try {
    if ($archive.Entries.Count -ne $entries.Count) { throw 'ZIP entry count mismatch.' }
    foreach ($entry in $archive.Entries) {
      if (-not $entries.ContainsKey($entry.FullName) -or $entry.FullName.Contains('\')) {
        throw "Unexpected ZIP entry: $($entry.FullName)"
      }
      if ($entry.Length -ne $entries[$entry.FullName].Length) { throw "ZIP size mismatch: $($entry.FullName)" }
      $stream = $entry.Open()
      $sha = [Security.Cryptography.SHA256]::Create()
      try {
        $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
        $expected = (Get-FileHash -LiteralPath $entries[$entry.FullName].FullName -Algorithm SHA256).Hash
        if ($actual -ne $expected) { throw "ZIP content mismatch: $($entry.FullName)" }
      } finally { $stream.Dispose(); $sha.Dispose() }
    }
  } finally { $archive.Dispose() }
}
