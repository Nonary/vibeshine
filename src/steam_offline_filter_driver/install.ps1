[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InfPath,
    [string]$ServiceName = 'VibeshineSteamOfflineFilter'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $InfPath -PathType Leaf)) { throw "Driver INF is missing: $InfPath" }
$bfe = Get-Service -Name BFE -ErrorAction Stop
if ($bfe.Status -ne 'Running') {
    $bfe.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
}
if ($bfe.Status -ne 'Running') { throw 'Base Filtering Engine is not ready.' }
# Stage the signed package, then process DefaultInstall.Services explicitly.
# /install only binds a package to matching devnodes; this is a devnode-free
# kernel service and must be installed through SetupAPI before Start-Service.
& pnputil.exe /add-driver $InfPath
if ($LASTEXITCODE -ne 0) { throw "pnputil driver-store import failed with exit code $LASTEXITCODE" }
$setupApi = Join-Path $env:SystemRoot 'System32\rundll32.exe'
$setup = Start-Process -FilePath $setupApi -ArgumentList @(
    'setupapi.dll,InstallHinfSection', 'DefaultInstall', '132', $InfPath
) -Wait -PassThru -WindowStyle Hidden
if ($setup.ExitCode -ne 0) { throw "SetupAPI DefaultInstall failed with exit code $($setup.ExitCode)" }
$service = Get-Service -Name $ServiceName -ErrorAction Stop
if ($service.Status -ne 'Running') { Start-Service -Name $ServiceName -ErrorAction Stop }
$service.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
if ($service.Status -ne 'Running') { throw "Driver service did not reach Running: $ServiceName" }
$handle = $null
try {
    $handle = [IO.File]::Open('\\.\VibeshineSteamOfflineFilter', [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
} finally {
    if ($handle) { $handle.Dispose() }
}
Write-Output 'Steam Offline Filter device is ready.'
