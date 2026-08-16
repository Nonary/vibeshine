param(
    [Parameter(Mandatory = $true)][string]$ExtractRoot,
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$SourceTrustedRoot,
    [Parameter(Mandatory = $true)][string]$SourcePackageRoot
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vdd_package_security.ps1')

$extractFull = Get-VddFullPath $ExtractRoot
Assert-VddNoReparseTree -Root $extractFull -TrustedRoot $extractFull -RequireConsistentOwner | Out-Null
$manifest = Read-VddManifest -ManifestPath $ManifestPath -TrustedBuildRoot $SourceTrustedRoot

$catalogs = @(Get-ChildItem -LiteralPath $extractFull -Recurse -File -Force -ErrorAction Stop |
    Where-Object { $_.Name -ieq 'SunshineVirtualDisplayDriver.cat' })
if ($catalogs.Count -ne 1) {
    throw "Extracted MSI must contain exactly one SunshineVirtualDisplayDriver.cat; found $($catalogs.Count)."
}
$packageRoots = @($catalogs | ForEach-Object { $_.Directory.FullName } | Sort-Object -Unique)
if ($packageRoots.Count -ne 1) {
    throw "Extracted MSI must contain exactly one Sunshine virtual-display package root; found $($packageRoots.Count)."
}
$packageRoot = Assert-VddSafePathComponents -Root $extractFull -Path $packageRoots[0]
Assert-VddManifestPayload -Manifest $manifest -PackageRoot $packageRoot -TrustedRoot $extractFull -VerifyCatalog
Assert-VddSourceOwnedFiles -Manifest $manifest -SourcePackageRoot $SourcePackageRoot -PackageRoot $packageRoot -SourceTrustedRoot $SourceTrustedRoot -PackageTrustedRoot $extractFull

Write-Host "[SunshineVirtualDisplay] Extracted MSI payload matches the checked-in manifest, catalog, and source-owned files: $packageRoot"
