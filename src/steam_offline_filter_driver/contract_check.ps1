[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$abi = Get-Content -LiteralPath (Join-Path $root 'steam_offline_filter_ioctl_abi.h') -Raw
$shared = Get-Content -LiteralPath (Join-Path $root '..\steam_offline_filter_ioctl.h') -Raw
$driver = Get-Content -LiteralPath (Join-Path $root 'steam_offline_filter.c') -Raw
$driverHeader = Get-Content -LiteralPath (Join-Path $root 'steam_offline_filter.h') -Raw
$isolation = Get-Content -LiteralPath (Join-Path $root '..\steam_offline_isolation.cpp') -Raw
$worker = Get-Content -LiteralPath (Join-Path $root '..\terminal_session_worker_process.cpp') -Raw
$inf = Get-Content -LiteralPath (Join-Path $root 'SteamOfflineFilter.inf') -Raw
$readme = Get-Content -LiteralPath (Join-Path $root 'README.md') -Raw
$progress = Get-Content -LiteralPath (Join-Path $root '..\..\docs\duo_session\PROGRESS.md') -Raw
$windowsCmake = Get-Content -LiteralPath (Join-Path $root '..\..\cmake\packaging\windows.cmake') -Raw
$windowsWix = Get-Content -LiteralPath (Join-Path $root '..\..\cmake\packaging\windows_wix.cmake') -Raw
$windowsTargets = Get-Content -LiteralPath (Join-Path $root '..\..\cmake\targets\windows.cmake') -Raw
$driverCmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$wixActions = Get-Content -LiteralPath (Join-Path $root '..\..\packaging\windows\wix\custom_actions.wxs') -Raw
$wixPatch = Get-Content -LiteralPath (Join-Path $root '..\..\packaging\windows\wix\patch_custom_actions.wxs') -Raw
$wixPayload = Get-Content -LiteralPath (Join-Path $root 'VibeshineSteamOfflineFilter.wxs') -Raw
$packageScript = Get-Content -LiteralPath (Join-Path $root 'package.ps1') -Raw
$installScript = Get-Content -LiteralPath (Join-Path $root 'install.ps1') -Raw
$uninstallScript = Get-Content -LiteralPath (Join-Path $root 'uninstall.ps1') -Raw

foreach ($needle in @('processCreationTime', 'generation', 'seatId', 'readinessGeneration', 'STEAM_OFFLINE_STATUS_IOCTL', 'bfeReady', 'wfpReady', 'STEAM_OFFLINE_MAX_SEAT_ID_SIZE 64u')) {
    if ($abi -notmatch [regex]::Escape($needle)) { throw "ABI mirror missing $needle" }
}
foreach ($needle in @('struct unregister_root_t', 'struct registration_t', 'struct status_t', 'status_ioctl', 'std::uint64_t generation', 'readiness_generation', 'char seat_id[max_seat_id_size]')) {
    if ($shared -notmatch [regex]::Escape($needle)) { throw "Shared unregister ABI missing $needle" }
}
foreach ($needle in @('process_creation_time', 'generation', 'seat_id', 'status_ioctl', 'bfe_ready', 'wfp_ready', 'readiness_generation', 'max_seat_id_size = 64')) {
    if ($shared -notmatch [regex]::Escape($needle)) { throw "Shared ABI missing $needle" }
}
foreach ($needle in @(
    'FWPM_LAYER_ALE_AUTH_CONNECT_V4',
    'FWPM_LAYER_ALE_AUTH_CONNECT_V6',
    'GUID_STEAM_OFFLINE_SUBLAYER',
    'FwpmSubLayerAdd0',
    'FwpmSubLayerDeleteByKey0',
    'FWP_ACTION_CALLOUT_UNKNOWN',
    'FWPM_FILTER_FLAG_PERMIT_IF_CALLOUT_UNREGISTERED',
    'PsSetCreateProcessNotifyRoutineEx',
    'PsGetProcessCreateTimeQuadPart',
    'FWPS_METADATA_FIELD_PROCESS_PATH',
    'steam.exe',
    'steamwebhelper.exe',
    'GameOverlayUI.exe',
    'steamerrorreporter.exe',
    'steamerrorreporter64.exe',
    'D:P(A;;GA;;;SY)')) {
    if ($driver -notmatch [regex]::Escape($needle)) { throw "Driver contract missing $needle" }
}
foreach ($needle in @(
    'VibeshineSteamOfflineFilter',
    'FWP_ACTION_CONTINUE',
    'FWPS_RIGHT_ACTION_WRITE',
    'SteamOfflineFoldAscii',
    'SteamOfflineUnicodeCodeUnitEqualsInsensitive',
    'SteamOfflineUnicodeEqualsAsciiInsensitive',
    'STEAM_OFFLINE_CANONICAL_IMAGE',
    'SteamOfflineCanonicalizeImagePath',
    '_IRQL_requires_(PASSIVE_LEVEL)',
    '_IRQL_requires_max_(DISPATCH_LEVEL)',
    'CreatingThreadId.UniqueProcess',
    'FileOpenNameAvailable',
    'SeLocateProcessImageName',
    'CreationStatus',
    'IRP_MJ_CLEANUP',
    'SteamOfflineCleanupRegistration',
    'SteamOfflineCountRegistrationProcessesLocked',
    'FwpmBfeStateGet',
    'FwpmBfeStateSubscribeChanges',
    'FwpmBfeStateUnsubscribeChanges',
    'gBfeReady',
    'gWfpReady',
    'gWfpStateLock',
    'gWfpStopping',
    'gWfpPublishedGeneration',
    'gBfeCallbackInFlight',
    'readinessGeneration',
    'SteamOfflineRemoveProcessNotify',
    'SteamOfflineUnregisterCallout',
    'SteamOfflineCallbacksRemain',
    'rejected',
    'MAX_REGISTRATIONS',
    'gRegistrations',
    'seatId',
    'generation')) {
    if ($driver -notmatch [regex]::Escape($needle)) { throw "Driver arbitration/lifetime contract missing $needle" }
}
if ($driver -match 'createInfo->ParentProcessId') { throw 'Driver must not trust PS_CREATE_NOTIFY_INFO.ParentProcessId.' }
if ($driver -match 'FWPM_SUBLAYER_UNIVERSAL') {
    throw 'Steam Offline filters must use the private Vibeshine sublayer.'
}
if ($driver -notmatch 'filter\.subLayerKey\s*=\s*GUID_STEAM_OFFLINE_SUBLAYER' -or
    $driver -notmatch 'subLayer\.weight\s*=\s*0x8000') {
    throw 'Private sublayer linkage/weight contract is incomplete.'
}
$filterDelete = $driver.IndexOf('FwpmFilterDeleteById0')
$calloutObjectDelete = $driver.IndexOf('FwpmCalloutDeleteByKey0')
$subLayerDelete = $driver.IndexOf('FwpmSubLayerDeleteByKey0')
if ($filterDelete -lt 0 -or $calloutObjectDelete -lt $filterDelete -or $subLayerDelete -lt $calloutObjectDelete) {
    throw 'WFP stop/rollback must delete filters, callout objects, then the private sublayer.'
}
if ($driver -match 'RtlEqualUnicodeString') {
    throw 'PASSIVE_LEVEL-only RtlEqualUnicodeString must not be used by the driver path.'
}
foreach ($helperName in @('SteamOfflineClassify', 'SteamOfflinePathEqualsBlob', 'SteamOfflineIsSteamClientPath', 'SteamOfflineInsertProcessLocked')) {
    $helperStart = $driver.IndexOf($helperName)
    if ($helperStart -lt 0) { throw "Driver helper missing: $helperName" }
    $helperEnd = $driver.IndexOf('static ', $helperStart + $helperName.Length)
    if ($helperEnd -lt 0) { $helperEnd = $driver.Length }
    $helperText = $driver.Substring($helperStart, $helperEnd - $helperStart)
    $forbiddenNames = @('RtlEqualUnicodeString', 'SeLocateProcessImageName', 'ZwQueryInformationProcess', 'PsLookupProcessByProcessId')
    if ($helperName -eq 'SteamOfflineInsertProcessLocked') {
        $forbiddenNames += @('UNICODE_STRING', 'SteamOfflineCopyImagePath', 'SteamOfflineIsSafeImagePath', 'SteamOfflineIsSteamClientPath')
    }
    foreach ($forbidden in $forbiddenNames) {
        if ($helperText -match [regex]::Escape($forbidden)) {
            throw "$helperName reaches a pageable/identity routine: $forbidden"
        }
    }
}
if ($driver -match 'action\.type\s*=\s*FWP_ACTION_CALLOUT_TERMINATING') {
    throw 'FWP_ACTION_CALLOUT_TERMINATING is incompatible with SteamOfflineClassify FWP_ACTION_CONTINUE permits.'
}
if ($driver -match '\(void\)SteamOfflineStopWfp\s*\(') {
    throw 'Startup rollback must check SteamOfflineStopWfp before returning.'
}
$startWfpStart = $driver.IndexOf('static NTSTATUS SteamOfflineStartWfp(_In_ PDEVICE_OBJECT deviceObject)')
$startWfpEnd = $driver.IndexOf('static NTSTATUS SteamOfflineUnregisterCallout', $startWfpStart + 1)
if ($startWfpStart -lt 0 -or $startWfpEnd -le $startWfpStart -or
    $driver.Substring($startWfpStart, $startWfpEnd - $startWfpStart) -match 'status\s*=\s*cleanupStatus') {
    throw 'Startup rollback must preserve the original startup failure; cleanup diagnostics are log-only.'
}
$stopWfpStart = $driver.IndexOf('static NTSTATUS SteamOfflineStopWfp(_Out_opt_ BOOLEAN* ownershipRemaining)')
$stopWfpEnd = $driver.IndexOf('static NTSTATUS SteamOfflineRemoveProcessNotify', $stopWfpStart + 1)
if ($stopWfpStart -lt 0 -or $stopWfpEnd -le $stopWfpStart) {
    throw 'WFP cleanup must expose authoritative ownership separately from its diagnostic status.'
}
$stopWfpText = $driver.Substring($stopWfpStart, $stopWfpEnd - $stopWfpStart)
foreach ($needle in @('ownershipRemaining', 'SteamOfflineCallbacksRemain()', 'FwpmBfeStateUnsubscribeChanges', 'gBfeCallbackInFlight')) {
    if ($stopWfpText -notmatch [regex]::Escape($needle)) {
        throw "WFP cleanup ownership contract missing $needle"
    }
}
$unloadStart = $driver.IndexOf('void SteamOfflineUnload')
$unloadEnd = $driver.IndexOf('NTSTATUS DriverEntry', $unloadStart + 1)
$unloadText = $driver.Substring($unloadStart, $unloadEnd - $unloadStart)
if ($unloadText -notmatch 'if \(gProcessNotifyRegistered\)' -or
    $unloadText -notmatch 'SteamOfflineStopWfp\(&ownershipRemaining\)' -or
    $unloadText -notmatch 'ownershipRemaining \|\| SteamOfflineCallbacksRemain\(\)' -or
    $unloadText -notmatch 'diagnosticStatus') {
    throw 'Unload must stop WFP after process-notify removal and gate device teardown on ownership, not diagnostics.'
}
$entryStart = $driver.IndexOf('NTSTATUS DriverEntry')
$entryText = $driver.Substring($entryStart)
if ($entryText -notmatch 'SteamOfflineCallbacksRemain\(\)' -or
    $entryText -notmatch 'IoDeleteSymbolicLink\(&dosName\)' -or
    $entryText -notmatch 'Flags \|= DO_DEVICE_INITIALIZING' -or
    $entryText -notmatch 'return STATUS_SUCCESS') {
    throw 'DriverEntry must retain a non-openable dormant image and return success when callback ownership remains.'
}
$startupRollbackStart = $entryText.IndexOf('status = SteamOfflineStartWfp')
$startupRollbackEnd = $entryText.IndexOf('    /* A successfully running WFP callout', $startupRollbackStart)
$startupRollbackText = $entryText.Substring($startupRollbackStart, $startupRollbackEnd - $startupRollbackStart)
$ownershipBranch = $startupRollbackText.IndexOf('if (SteamOfflineCallbacksRemain())')
$ownershipBranchSuccess = $startupRollbackText.IndexOf('return STATUS_SUCCESS', $ownershipBranch)
$ownershipBranchFailure = $startupRollbackText.IndexOf('return status;', $ownershipBranch)
if ($ownershipBranch -lt 0 -or $ownershipBranchSuccess -lt 0 -or
    ($ownershipBranchFailure -ge 0 -and $ownershipBranchFailure -lt $ownershipBranchSuccess)) {
    throw 'DriverEntry must not return failure after retaining callback ownership.'
}
$statusBlock = $driver.Substring($driver.IndexOf('case STEAM_OFFLINE_STATUS_IOCTL'),
    $driver.IndexOf('default:', $driver.IndexOf('case STEAM_OFFLINE_STATUS_IOCTL')) - $driver.IndexOf('case STEAM_OFFLINE_STATUS_IOCTL'))
if ($statusBlock -notmatch 'currentGeneration == publishedGeneration' -or
    $statusBlock -notmatch 'publishedReady') {
    throw 'Status IOCTL must expose only generation-consistent published readiness.'
}
$registerBlockStart = $driver.IndexOf('static NTSTATUS SteamOfflineRegisterRoot')
$registerBlockEnd = $driver.IndexOf('static NTSTATUS SteamOfflineUnregisterRoot', $registerBlockStart)
if ($registerBlockStart -lt 0 -or $registerBlockEnd -le $registerBlockStart) { throw 'Registration block missing.' }
$registerBlock = $driver.Substring($registerBlockStart, $registerBlockEnd - $registerBlockStart)
if ($registerBlock -notmatch 'gWfpPublishedGeneration' -or
    $registerBlock -notmatch 'readinessGeneration') {
    throw 'Registration admission must bind the published readiness generation.'
}
$stopLockedStart = $driver.IndexOf('SteamOfflineStopWfpLocked')
$stopLockedEnd = $driver.IndexOf('static NTSTATUS SteamOfflineStopWfp(_Out_opt_ BOOLEAN* ownershipRemaining)', $stopLockedStart + 1)
if ($stopLockedStart -ge 0 -and $stopLockedEnd -gt $stopLockedStart -and
    $driver.Substring($stopLockedStart, $stopLockedEnd - $stopLockedStart) -match 'FwpmBfeStateUnsubscribeChanges') {
    throw 'FwpmBfeStateUnsubscribeChanges must run after releasing gWfpStateLock.'
}
$rightsCheck = $driver.IndexOf('canWrite = (classifyOut->rights & FWPS_RIGHT_ACTION_WRITE) != 0;')
$continueAction = $driver.IndexOf('classifyOut->actionType = FWP_ACTION_CONTINUE;')
$metadataCheck = $driver.IndexOf('FWPS_METADATA_FIELD_PROCESS_ID')
if ($rightsCheck -lt 0 -or $continueAction -le $rightsCheck -or
    ($metadataCheck -ge 0 -and $metadataCheck -lt $continueAction)) {
    throw 'SteamOfflineClassify must initialize writable-path CONTINUE immediately after checking rights.'
}
if (-not ($driver -match 'classifyOut->actionType == FWP_ACTION_PERMIT') -or
    -not ($driver -match 'canWrite')) {
    throw 'SteamOfflineClassify must arbitrate exact matches even when action-write rights are absent.'
}
foreach ($needle in @('STEAM_OFFLINE_MAX_PROCESSES_PER_REGISTRATION', 'terminated')) {
    if ($driverHeader -notmatch [regex]::Escape($needle) -and $driver -notmatch [regex]::Escape($needle)) {
        throw "Per-registration seat/padding contract missing $needle"
    }
}
if ($isolation -notmatch 'VibeshineSteamOfflineFilter' -or
    $isolation -notmatch 'FILE_SHARE_READ' -or
    $isolation -notmatch 'FILE_SHARE_WRITE') {
    throw 'User-mode device name/share contract is incomplete.'
}
if ($readme -notmatch 'no supported `PsGetProcessJob`' -or
    $readme -notmatch 'PsGetProcessSessionId' -or
    $readme -notmatch 'ProcessSessionInformation' -or
    $readme -notmatch 'non-standard process-clone') {
    throw 'Unsupported session/job membership and clone limitations must be documented explicitly.'
}
if ($readme -match 'build-enabled MSI installs' -or $progress -match 'INSTALL_STEAM_OFFLINE_FILTER' -or
    $progress -match 'build-enabled MSI carries') {
    throw 'Documentation must not claim an active MSI runtime-property/package lane.'
}
$contracts = @(
    [PSCustomObject]@{ Text = $windowsCmake; Needles = @('SUNSHINE_BUILD_STEAM_OFFLINE_FILTER_DRIVER', '%13%') },
    [PSCustomObject]@{ Text = $driverCmake; Needles = @('SUNSHINE_STEAM_OFFLINE_FILTER_INFVERIF_PATH', 'SUNSHINE_STEAM_OFFLINE_FILTER_EXPECTED_PUBLISHER', 'SUNSHINE_STEAM_OFFLINE_FILTER_EXPECTED_ISSUER', '-InfVerifPath', '-ExpectedPublisher', '-ExpectedIssuer') },
    [PSCustomObject]@{ Text = $windowsTargets; Needles = @('SUNSHINE_BUILD_STEAM_OFFLINE_FILTER_DRIVER', 'two-phase reboot transaction', 'FATAL_ERROR') },
    [PSCustomObject]@{ Text = $windowsWix; Needles = @('SteamOfflineFilterPayload', 'VibeshineSteamOfflineFilter.wxs', 'two-phase reboot transaction', 'FATAL_ERROR') },
    [PSCustomObject]@{ Text = $wixActions; Needles = @('InstallSteamOfflineFilter', 'UninstallSteamOfflineFilter') },
    [PSCustomObject]@{ Text = $wixPatch; Needles = @('RollbackSteamOfflineFilter', 'RollbackUninstallSteamOfflineFilter', '?SteamOfflineFilterPayload = 3', 'WIX_UPGRADE_DETECTED') },
    [PSCustomObject]@{ Text = $wixPayload; Needles = @('SteamOfflineFilterPayload', '4C018C93-B0A7-4A5B-8A6D-7E3F7C8A4D11', 'uninstall.ps1') },
    [PSCustomObject]@{ Text = $packageScript; Needles = @('InfVerif', 'Inf2Cat', 'Get-AuthenticodeSignature', 'VibeshineSteamOfflineFilter.cat', '%13%', 'ExpectedPublisher', 'ExpectedIssuer') },
    [PSCustomObject]@{ Text = $installScript; Needles = @('InstallHinfSection', 'pnputil.exe /add-driver $InfPath') },
    [PSCustomObject]@{ Text = $uninstallScript; Needles = @('sc.exe delete', 'delete-driver', '$rebootRequired', 'reboot is required') }
)
foreach ($contract in $contracts) {
    foreach ($needle in $contract.Needles) {
        if ($contract.Text -notmatch [regex]::Escape($needle)) { throw "Packaging contract missing $needle" }
    }
}
if ($wixPatch -match 'Action="(?:Set)?UninstallSteamOfflineFilter"[^>]*>WIX_UPGRADE_DETECTED\s+OR\s+\(REMOVE') {
    throw 'Steam offline uninstall/upgrade actions must require the installed payload component.'
}
if ($wixPayload -match '<Condition>') { throw 'Build-enabled Steam offline payload must not be runtime-property conditional.' }
if ($wixPatch -match 'INSTALL_STEAM_OFFLINE_FILTER') { throw 'MSI must not reset runtime Steam offline isolation with an install property.' }
if ($packageScript.IndexOf('/driver:$out') -lt $packageScript.IndexOf('sign /fd SHA256 /sha1 $thumbprint $sysOutput')) {
    throw 'Package flow must sign SYS before Inf2Cat hashes the driver.'
}
foreach ($needle in @('cleanup_pending_', 'cleanup_needed()', 'steam_offline_registration_.active()', 'reconnect is blocked')) {
    if ($worker -notmatch [regex]::Escape($needle)) { throw "Worker cleanup gate missing $needle" }
}
if ($worker -notmatch 'steam_offline_registration_\.healthy\(readiness_error\)' -or
    $worker -notmatch 'TerminateJobObject\(job, ERROR_NETWORK_NOT_AVAILABLE\)' -or
    $worker -notmatch 'WaitForSingleObject\(process, 5000\)') {
    throw 'Worker must prove readiness generation before resume and verify exact-job containment on loss.'
}
$preResumeHealth = $worker.IndexOf('steam_offline_registration_.healthy(readiness_error)')
$resume = $worker.IndexOf('ResumeThread(info.hThread)')
if ($preResumeHealth -lt 0 -or $resume -lt 0 -or $preResumeHealth -gt $resume) {
    throw 'Worker readiness proof must precede ResumeThread.'
}
if ($driver -match 'steamservice\.exe') { throw 'Machine-global steamservice.exe must not be a client image.' }
foreach ($needle in @('Class = System', 'ClassGuid = {4d36e97d-e325-11ce-bfc1-08002be10318}',
        'StartType = 3', 'DependOnService = BFE', 'PnpLockdown = 1', 'CatalogFile',
        'ServiceBinary = %13%\VibeshineSteamOfflineFilter.sys', '[DefaultInstall.NTamd64.Services]')) {
    if ($inf -notmatch [regex]::Escape($needle)) { throw "INF contract missing $needle" }
}
if ($inf -match 'DefaultUninstall') { throw 'Devnode-free INF must not rely on prohibited DefaultUninstall sections.' }
foreach ($needle in @('FwpmBfeStateSubscribeChanges', 'FwpmBfeStateUnsubscribeChanges', 'gBfeReady', 'FwpmBfeStateGet')) {
    if ($driver -notmatch [regex]::Escape($needle)) { throw "BFE readiness contract missing $needle" }
}
Write-Output 'Steam Offline Filter static contract passed.'
