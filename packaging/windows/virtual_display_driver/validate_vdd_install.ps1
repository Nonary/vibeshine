param(
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$SourceManifestPath,
    [Parameter(Mandatory = $true)][string]$SourceTrustedRoot,
    [Parameter(Mandatory = $true)][string]$SourcePackageRoot,
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$InstalledPackageRoot,
    [Parameter(Mandatory = $true)][string]$TrustedBuildRoot,
    [switch]$Preflight,
    [switch]$RequirePinnedProvenance
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vdd_package_security.ps1')

$sourceManifest = Read-VddManifest -ManifestPath $SourceManifestPath -TrustedBuildRoot $SourceTrustedRoot
$buildManifest = Read-VddManifest -ManifestPath $ManifestPath -TrustedBuildRoot $TrustedBuildRoot
$sourceHash = (Get-FileHash -LiteralPath $sourceManifest.Path -Algorithm SHA256).Hash.ToLowerInvariant()
$buildHash = (Get-FileHash -LiteralPath $buildManifest.Path -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sourceHash -ne $buildHash) {
    throw "Build-local VDD manifest does not exactly match the checked-in source manifest."
}
# Use the checked-in manifest object for all subsequent expectations. The
# build-local copy is only accepted after this exact hash comparison.
$manifest = $sourceManifest
$packageFull = Assert-VddSafePathComponents -Root $TrustedBuildRoot -Path $PackageRoot
$readyPath = Join-Path $packageFull 'libvirtualdisplay-ready.json'
Assert-VddReadyRecord -Manifest $manifest -PackageRoot $packageFull -TrustedRoot $TrustedBuildRoot -SourcePackageRoot $SourcePackageRoot -SourceTrustedRoot $SourceTrustedRoot -ReadyPath $readyPath -RequirePinnedProvenance:$RequirePinnedProvenance

$installParent = Split-Path -Parent (Get-VddFullPath $InstalledPackageRoot)
Assert-VddSafePathComponents -Root $TrustedBuildRoot -Path $installParent | Out-Null
if ($Preflight) {
    if (Test-Path -LiteralPath $InstalledPackageRoot) { Assert-VddNoReparseTree -Root $InstalledPackageRoot -TrustedRoot $TrustedBuildRoot -RequireConsistentOwner | Out-Null }
    Write-Host '[SunshineVirtualDisplay] VDD pre-install destination and pinned provenance validated.'
    return
}
if (-not (Test-Path -LiteralPath $InstalledPackageRoot -PathType Container)) {
    throw "Installed VDD package root is missing: $InstalledPackageRoot"
}
Assert-VddManifestPayload -Manifest $manifest -PackageRoot $InstalledPackageRoot -TrustedRoot $TrustedBuildRoot -VerifyCatalog
Assert-VddSourceOwnedFiles -Manifest $manifest -SourcePackageRoot $SourcePackageRoot -PackageRoot $InstalledPackageRoot -SourceTrustedRoot $SourceTrustedRoot -PackageTrustedRoot $TrustedBuildRoot
Write-Host "[SunshineVirtualDisplay] Installed package matches pinned VDD manifest and ready record."
