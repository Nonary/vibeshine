[CmdletBinding()]
param(
    [string]$ServiceName = 'VibeshineSteamOfflineFilter'
)

$ErrorActionPreference = 'Stop'
$rebootRequired = $false
$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped') {
    try {
        Stop-Service -Name $ServiceName -Force -ErrorAction Stop
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    } catch {
        # A successfully started callout driver deliberately has no unload
        # entry point.  Keep the service/file removal staged rather than
        # claiming it stopped; the reboot is the callback-drain boundary.
        $rebootRequired = $true
    }
    $service.Refresh()
    if ($service.Status -ne 'Stopped') { $rebootRequired = $true }
}
if ($service) {
    & sc.exe delete $ServiceName
    if ($LASTEXITCODE -eq 1072) { $rebootRequired = $true }
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1060 -and $LASTEXITCODE -ne 1072) {
        throw "Driver service removal failed with exit code $LASTEXITCODE"
    }
}
$packages = @(Get-WindowsDriver -Online -ErrorAction SilentlyContinue |
    Where-Object { $_.OriginalFileName -match '(?i)SteamOfflineFilter\.inf' })
foreach ($package in $packages) {
    & pnputil.exe /delete-driver $package.PublishedName /uninstall /force
    if ($LASTEXITCODE -ne 0) { throw "pnputil driver removal failed for $($package.PublishedName)" }
}
if ($rebootRequired) {
    Write-Output 'Steam Offline Filter driver removal staged; reboot is required before the old image and callbacks are gone.'
} else {
    Write-Output 'Steam Offline Filter driver removed; no WFP callout remains active.'
}
