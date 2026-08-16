param(
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [Parameter(Mandatory = $true)][string]$TrustedBuildRoot,
    [Parameter(Mandatory = $true)][string]$SourceManifestPath,
    [Parameter(Mandatory = $true)][string]$SourceTrustedRoot,
    [string]$GitHubToken = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\packaging\windows\virtual_display_driver\vdd_package_security.ps1')

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host "[libvirtualdisplay] $Message"
}

function New-GitHubHeaders {
    param([string]$Token = '')
    $headers = @{
        Accept = 'application/vnd.github+json'
        'User-Agent' = 'vibeshine-libvirtualdisplay-downloader'
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    if ($Token) { $headers.Authorization = "Bearer $Token" }
    return $headers
}

function Test-CompleteCache {
    param([Parameter(Mandatory = $true)]$Manifest, [Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$TrustedRoot)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    $cacheManifest = Join-Path $Path 'libvirtualdisplay-manifest.json'
    if (-not (Test-Path -LiteralPath $cacheManifest -PathType Leaf)) { return $false }
    if ((Get-FileHash -LiteralPath $Manifest.Path -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $cacheManifest -Algorithm SHA256).Hash) { return $false }
    Assert-VddManifestPayload -Manifest $Manifest -PackageRoot $Path -TrustedRoot $TrustedRoot -PrebuiltLayout -VerifyCatalog
    return $true
}

$manifest = Read-VddManifest -ManifestPath $ManifestPath -TrustedBuildRoot $TrustedBuildRoot
$sourceManifest = Read-VddManifest -ManifestPath $SourceManifestPath -TrustedBuildRoot $SourceTrustedRoot
if ((Get-FileHash $manifest.Path -Algorithm SHA256).Hash -ne (Get-FileHash $sourceManifest.Path -Algorithm SHA256).Hash) { throw 'Build-local VDD manifest differs from the checked-in source manifest.' }
Assert-VddManifestIdentity -Manifest $manifest -Repository $Repository -Tag $Tag
$OutDir = Assert-VddContainedPath -Root $TrustedBuildRoot -Path $OutDir
if (-not $GitHubToken) {
    $GitHubToken = if ($env:GH_TOKEN) { $env:GH_TOKEN } elseif ($env:GITHUB_TOKEN) { $env:GITHUB_TOKEN } else { '' }
}

if (Test-Path -LiteralPath $OutDir) {
    try {
        if (Test-CompleteCache -Manifest $manifest -Path $OutDir -TrustedRoot $TrustedBuildRoot) {
            Write-Step "Using verified $Repository release $Tag from $OutDir"
            exit 0
        }
        Write-Step "Rejecting incomplete or stale cache at $OutDir; downloading a fresh package"
    } catch {
        if ($_.Exception.Message -match 'reparse|escapes trusted root|Refusing to remove') { throw }
        Write-Step "Rejecting unverifiable cache at $OutDir; downloading a fresh package"
    }
}

$headers = New-GitHubHeaders -Token $GitHubToken
$releaseUri = "https://api.github.com/repos/$Repository/releases/tags/$([System.Uri]::EscapeDataString($Tag))"
Write-Step "Resolving $Repository release $Tag"
try {
    $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
} catch {
    if (-not $GitHubToken) { throw }
    Write-Step 'Authenticated release lookup failed; retrying public lookup without a token'
    $release = Invoke-RestMethod -Uri $releaseUri -Headers (New-GitHubHeaders)
}

$commitUri = "https://api.github.com/repos/$Repository/commits/$([System.Uri]::EscapeDataString($Tag))"
$releaseCommit = Invoke-RestMethod -Uri $commitUri -Headers (New-GitHubHeaders -Token $GitHubToken)
if ($releaseCommit.sha -ne $manifest.Data.commit) {
    throw "Release '$Repository@$Tag' resolves to commit '$($releaseCommit.sha)', not the manifest commit '$($manifest.Data.commit)'."
}

$assetName = [string]$manifest.Data.asset.name
$asset = @($release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1)
if ($asset.Count -ne 1) { throw "Release '$Repository@$Tag' does not contain asset '$assetName'." }

$workRoot = New-VddSiblingPath -Path $OutDir -TrustedRoot $TrustedBuildRoot -Suffix 'download'
$stageDir = New-VddSiblingPath -Path $OutDir -TrustedRoot $TrustedBuildRoot -Suffix 'stage'
New-Item -ItemType Directory -Path $workRoot, $stageDir -Force | Out-Null
try {
    $archivePath = Join-Path $workRoot $assetName
    $downloadHeaders = New-GitHubHeaders -Token $GitHubToken
    $downloadHeaders.Accept = 'application/octet-stream'
    Write-Step "Downloading $assetName"
    try {
        Invoke-WebRequest -Uri $asset.url -Headers $downloadHeaders -OutFile $archivePath
    } catch {
        if (-not $GitHubToken) { throw }
        Write-Step 'Authenticated asset download failed; retrying public download without a token'
        $downloadHeaders = New-GitHubHeaders
        $downloadHeaders.Accept = 'application/octet-stream'
        Invoke-WebRequest -Uri $asset.url -Headers $downloadHeaders -OutFile $archivePath
    }

    $archive = Get-Item -LiteralPath $archivePath -Force
    if ($archive.Length -ne [int64]$manifest.Data.asset.size) {
        throw "Release archive size $($archive.Length) does not match manifest $($manifest.Data.asset.size)."
    }
    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($archiveHash -ne $manifest.Data.asset.sha256.ToLowerInvariant()) {
        throw "Release archive SHA256 '$archiveHash' does not match the pinned manifest."
    }

    $extractDir = Join-Path $workRoot 'extract'
    New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
    Assert-VddZipEntryNames -ArchivePath $archivePath
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir -Force
    Assert-VddNoReparseTree -Root $extractDir -TrustedRoot $TrustedBuildRoot | Out-Null

    $packageRoot = @(
        Get-Item -LiteralPath $extractDir -Force
        Get-ChildItem -LiteralPath $extractDir -Directory -Recurse -Force
    ) | Where-Object {
        $candidate = $_.FullName
        @($manifest.Data.files | Where-Object {
            Test-Path -LiteralPath (Join-Path $candidate ($_.archive_path -replace '/', '\')) -PathType Leaf
        }).Count -eq @($manifest.Data.files).Count
    } | Select-Object -First 1
    if (-not $packageRoot) { throw "Release archive '$assetName' does not contain the pinned driver payload." }
    Assert-VddNoReparseTree -Root $packageRoot.FullName -TrustedRoot $TrustedBuildRoot | Out-Null

    foreach ($entry in @($manifest.Data.files)) {
        $source = Join-Path $packageRoot.FullName ($entry.archive_path -replace '/', '\')
        $destination = Join-Path $stageDir (Get-VddPayloadPath -Entry $entry -PrebuiltLayout)
        $parent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    Copy-Item -LiteralPath $manifest.Path -Destination (Join-Path $stageDir 'libvirtualdisplay-manifest.json') -Force
    Assert-VddManifestPayload -Manifest $manifest -PackageRoot $stageDir -TrustedRoot $TrustedBuildRoot -PrebuiltLayout -VerifyCatalog
    Move-VddDirectoryAtomically -Stage $stageDir -Destination $OutDir -TrustedRoot $TrustedBuildRoot
    Assert-VddManifestPayload -Manifest $manifest -PackageRoot $OutDir -TrustedRoot $TrustedBuildRoot -PrebuiltLayout -VerifyCatalog
    $cacheManifest = Join-Path $OutDir 'libvirtualdisplay-manifest.json'
    if ((Get-FileHash -LiteralPath $manifest.Path -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $cacheManifest -Algorithm SHA256).Hash) {
        throw 'Published VDD cache manifest does not match the checked-in manifest.'
    }
    Write-Step "Staged verified $Repository release $Tag at $OutDir"
} finally {
    if (Test-Path -LiteralPath $stageDir) {
        try { Remove-VddVerifiedTree -Path $stageDir -TrustedRoot $TrustedBuildRoot } catch { }
    }
    if (Test-Path -LiteralPath $workRoot) {
        try { Remove-VddVerifiedTree -Path $workRoot -TrustedRoot $TrustedBuildRoot } catch { }
    }
}
