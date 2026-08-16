[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DriverRoot,
    [string]$BuildOutputDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('x64','ARM64')][string]$Architecture = 'x64',
    [string]$InfVerifPath,
    [string]$Inf2CatPath,
    [string]$SignToolPath,
    [Parameter(Mandatory = $true)][string]$CertificateThumbprint,
    [Parameter(Mandatory = $true)][string]$ExpectedPublisher,
    [Parameter(Mandatory = $true)][string]$ExpectedIssuer
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $DriverRoot).Path
$build = if ([string]::IsNullOrWhiteSpace($BuildOutputDirectory)) {
    Join-Path $root 'build'
} else {
    (Resolve-Path -LiteralPath $BuildOutputDirectory).Path
}
$out = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $out) {
    Get-ChildItem -LiteralPath $out -Force | Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $out -Force | Out-Null
}
$sys = Join-Path $build 'VibeshineSteamOfflineFilter.sys'
$inf = Join-Path $root 'SteamOfflineFilter.inf'
function Assert-NonEmptyFile([string]$path, [string]$description) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing ${description}: ${path}" }
    if ((Get-Item -LiteralPath $path).Length -le 0) { throw "Empty ${description}: ${path}" }
}
Assert-NonEmptyFile $sys 'WDK output'
Assert-NonEmptyFile $inf 'INF'
$infVerif = $InfVerifPath
if ([string]::IsNullOrWhiteSpace($infVerif)) {
    $infVerif = (Get-Command InfVerif.exe -ErrorAction SilentlyContinue).Source
}
if ([string]::IsNullOrWhiteSpace($infVerif)) { throw 'InfVerif.exe is required to validate the non-PnP INF.' }
& $infVerif /info $inf
if ($LASTEXITCODE -ne 0) { throw "InfVerif failed with exit code $LASTEXITCODE" }
$infText = Get-Content -LiteralPath $inf -Raw
foreach ($required in @(
    'CatalogFile\s*=\s*VibeshineSteamOfflineFilter\.cat',
    'AddService\s*=\s*%ServiceName%',
    'StartType\s*=\s*3',
    'DependOnService\s*=\s*BFE',
    'PnpLockdown\s*=\s*1',
    'ServiceBinary\s*=\s*%13%\\VibeshineSteamOfflineFilter\.sys',
    'VibeshineSteamOfflineFilter\.sys')) {
    if ($infText -notmatch $required) { throw "INF contract missing $required" }
}
Assert-NonEmptyFile (Join-Path $root 'install.ps1') 'install script'
Assert-NonEmptyFile (Join-Path $root 'uninstall.ps1') 'uninstall script'
Copy-Item -LiteralPath $sys -Destination (Join-Path $out 'VibeshineSteamOfflineFilter.sys') -Force
Copy-Item -LiteralPath $inf -Destination (Join-Path $out 'SteamOfflineFilter.inf') -Force
Copy-Item -LiteralPath (Join-Path $root 'install.ps1') -Destination (Join-Path $out 'install.ps1') -Force
Copy-Item -LiteralPath (Join-Path $root 'uninstall.ps1') -Destination (Join-Path $out 'uninstall.ps1') -Force

if ([string]::IsNullOrWhiteSpace($SignToolPath)) {
    $SignToolPath = (Get-Command signtool.exe -ErrorAction SilentlyContinue).Source
}
if ([string]::IsNullOrWhiteSpace($SignToolPath)) {
    throw 'A repository-approved signtool is required; unsigned packages are not emitted.'
}
if ([string]::IsNullOrWhiteSpace($ExpectedPublisher) -or [string]::IsNullOrWhiteSpace($ExpectedIssuer)) {
    throw 'Expected signing publisher and issuer must be configured; retail/HVCI trust is not inferred by this local package lane.'
}
$thumbprint = ($CertificateThumbprint -replace '\s', '').ToUpperInvariant()
$certificate = Get-ChildItem -LiteralPath "Cert:\CurrentUser\My\$thumbprint" -ErrorAction SilentlyContinue
if (-not $certificate -or -not $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) {
    throw "Configured signing certificate is missing, private-key-less, or expired: $thumbprint"
}
if ($certificate.Subject -notlike "*$ExpectedPublisher*" -or $certificate.Issuer -notlike "*$ExpectedIssuer*") {
    throw "Configured signing certificate does not match expected publisher/issuer: $($certificate.Subject) / $($certificate.Issuer)"
}
$sysOutput = Join-Path $out 'VibeshineSteamOfflineFilter.sys'
# Sign the SYS before Inf2Cat so the catalog hashes the embedded-signed image.
& $SignToolPath sign /fd SHA256 /sha1 $thumbprint $sysOutput
if ($LASTEXITCODE -ne 0) { throw "Driver signing failed with exit code $LASTEXITCODE" }
$sysSignature = Get-AuthenticodeSignature -LiteralPath $sysOutput
if (-not $sysSignature.SignerCertificate -or $sysSignature.Status -eq 'NotSigned' -or
    (($sysSignature.SignerCertificate.Thumbprint -replace '\s', '').ToUpperInvariant() -ne $thumbprint)) {
    throw 'Embedded SYS signature does not match the configured certificate.'
}

if ([string]::IsNullOrWhiteSpace($Inf2CatPath)) {
    $Inf2CatPath = (Get-Command Inf2Cat.exe -ErrorAction SilentlyContinue).Source
}
if ([string]::IsNullOrWhiteSpace($Inf2CatPath)) { throw 'Inf2Cat.exe is required to produce a catalog.' }
$inf2CatOs = if ($Architecture -eq 'ARM64') { '10_ARM64,Server10_ARM64' } else { '10_X64,10_GE_X64,Server10_X64' }
& $Inf2CatPath "/driver:$out" "/os:$inf2CatOs"
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed with exit code $LASTEXITCODE" }
$cat = Join-Path $out 'VibeshineSteamOfflineFilter.cat'
Assert-NonEmptyFile $cat 'catalog'
& $SignToolPath sign /fd SHA256 /sha1 $thumbprint $cat
if ($LASTEXITCODE -ne 0) { throw "Catalog signing failed with exit code $LASTEXITCODE" }
foreach ($signed in @($cat, (Join-Path $out 'VibeshineSteamOfflineFilter.sys'))) {
    $signature = Get-AuthenticodeSignature -LiteralPath $signed
    if (-not $signature.SignerCertificate -or $signature.Status -eq 'NotSigned' -or
        (($signature.SignerCertificate.Thumbprint -replace '\s', '').ToUpperInvariant() -ne $thumbprint) -or
        $signature.SignerCertificate.Subject -notlike "*$ExpectedPublisher*" -or
        $signature.SignerCertificate.Issuer -notlike "*$ExpectedIssuer*") {
        throw "Signing did not produce the configured publisher/issuer signer for $signed"
    }
}
Write-Output "Steam Offline Filter package ready: $out"
