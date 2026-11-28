param(
  [Parameter(Mandatory = $true)][string]$StageDir,
  [Parameter(Mandatory = $true)][string]$SearchDirsJson
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'RuntimeDllClosure.ps1')
# Windows PowerShell 5 wraps the JSON array in another array with @(...).
$searchDirs = ConvertFrom-Json -InputObject $SearchDirsJson
Copy-RuntimeDllClosure -StageDir $StageDir -SearchDirs $searchDirs | Out-Host
Assert-RuntimeDllClosure -StageDir $StageDir | Out-Null
