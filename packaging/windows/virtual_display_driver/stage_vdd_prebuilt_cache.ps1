param(
    [Parameter(Mandatory = $true)][string]$SourcePackageDir,
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$TrustedBuildRoot,
    [Parameter(Mandatory = $true)][string]$SourceManifestPath,
    [Parameter(Mandatory = $true)][string]$SourceTrustedRoot
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vdd_package_security.ps1')

$manifest = Read-VddManifest -ManifestPath $ManifestPath -TrustedBuildRoot $TrustedBuildRoot
$sourceManifest = Read-VddManifest -ManifestPath $SourceManifestPath -TrustedBuildRoot $SourceTrustedRoot
if ((Get-FileHash $manifest.Path -Algorithm SHA256).Hash -ne (Get-FileHash $sourceManifest.Path -Algorithm SHA256).Hash) { throw 'Build-local VDD manifest differs from the checked-in source manifest.' }
$sourceFull = Get-VddFullPath $SourcePackageDir
$sourceParent = Split-Path -Parent $sourceFull
Assert-VddSafePathComponents -Root $sourceParent -Path $sourceFull | Out-Null
Assert-VddManifestPayload -Manifest $manifest -PackageRoot $sourceFull -TrustedRoot $sourceParent -PrebuiltLayout -VerifyCatalog

Copy-VddDirectoryAtomically `
    -Source $sourceFull `
    -Destination $PackageDir `
    -SourceTrustedRoot $sourceParent `
    -DestinationTrustedRoot $TrustedBuildRoot `
    -MarkerName '.cache-ready'

$published = Get-VddFullPath $PackageDir
Assert-VddManifestPayload -Manifest $manifest -PackageRoot $published -TrustedRoot $TrustedBuildRoot -PrebuiltLayout -VerifyCatalog
Assert-VddSafePathComponents -Root $TrustedBuildRoot -Path $published | Out-Null
Write-Host "[SunshineVirtualDisplay] Published caller-provided package into validated build cache: $published"
