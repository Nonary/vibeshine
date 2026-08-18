[CmdletBinding()]
param(
    [ValidateSet('Install', 'Rollback', 'Rearm', 'Verify')]
    [string] $Action = 'Install',
    [string] $InstallRoot,
    [string] $StateRoot = (Join-Path $env:ProgramData 'Vibeshine\TerminalIsolation')
)

# This is deliberately a test-only compatibility component.  It changes the
# TermService ServiceDll registry value and therefore has a boot boundary; it
# never restarts TermService or reboots Windows from an MSI custom action.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ServiceParametersPath = 'SYSTEM\CurrentControlSet\Services\TermService\Parameters'
$TaskName = '\Vibeshine\TerminalIsolation\Verify'
$StateFileName = 'state.json'
$StatusFileName = 'status.txt'
$PayloadDirectoryName = 'payload'
$OwnedMarker = 'Vibeshine-native-rdp-tcp-v1'
$StateSchema = 2
$TrustedOwnerSids = @(
    'S-1-5-18',
    'S-1-5-32-544',
    'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
$RuntimePinnedHashes = @{
    # Independent test-only compatibility pins.  A package manifest cannot
    # authorize a different wrapper or dependency merely by describing it.
    'TermWrap.dll' = '220f18e0b2c2091c5f684ec063c43831bffdf25e561bd123211cce883f8d25e2'
    'Zydis.dll' = '5908be0af05bf7584328cf5d0ddde2c108d693709ffc77a13822fdceB75797e1'.ToLowerInvariant()
    'LICENSE' = '72966f08ceaacf34475e7824ac566f2e966bef3c5e46a190dc844c1155486614'
}
$NativeTermsrvSha256 = 'f2d3150c45e3fe5dbf294abc6ed8326d6d36936fdff256f7e805cff55f9b1aea'
$NativeTermsrvFileVersion = '10.0.26100.8115'
$NativeTermsrvArchitecture = 'x64-System32'
$ExpectedStateRoot = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'Vibeshine\TerminalIsolation'))
if (-not [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($StateRoot), $ExpectedStateRoot)) {
    throw 'StateRoot must be the product-owned ProgramData terminal-isolation directory.'
}

function Fail([string] $Message) {
    try { Set-Status 'unavailable' } catch {}
    throw $Message
}

function Get-FullPath([string] $Path) {
    return [IO.Path]::GetFullPath($Path)
}

function Assert-NoReparsePath([string] $Path, [bool] $AllowMissingLeaf = $false) {
    $full = Get-FullPath $Path
    $current = $full
    $missingLeaf = $false
    while ($true) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing terminal-isolation path below a reparse point: $current"
            }
        } elseif ($current -eq $full -and $AllowMissingLeaf) {
            $missingLeaf = $true
        } else {
            throw "Required terminal-isolation path component is missing: $current"
        }
        $parent = Split-Path -LiteralPath $current -Parent
        if ([string]::IsNullOrEmpty($parent) -or $parent -eq $current) { break }
        $current = $parent
    }
}

function Assert-TrustedOwner([string] $Path) {
    $acl = Get-Acl -LiteralPath $Path -ErrorAction Stop
    $owner = $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin $TrustedOwnerSids) {
        throw "Refusing terminal-isolation mutation because the path owner is not a trusted Windows owner: $Path"
    }
    $writeRights = [System.Security.AccessControl.FileSystemRights]::Write -bor
        [System.Security.AccessControl.FileSystemRights]::WriteData -bor
        [System.Security.AccessControl.FileSystemRights]::AppendData -bor
        [System.Security.AccessControl.FileSystemRights]::CreateFiles -bor
        [System.Security.AccessControl.FileSystemRights]::CreateDirectories -bor
        [System.Security.AccessControl.FileSystemRights]::Delete -bor
        [System.Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
        [System.Security.AccessControl.FileSystemRights]::WriteAttributes -bor
        [System.Security.AccessControl.FileSystemRights]::WriteExtendedAttributes -bor
        [System.Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [System.Security.AccessControl.FileSystemRights]::TakeOwnership -bor
        [System.Security.AccessControl.FileSystemRights]::FullControl
    foreach ($entry in $acl.Access) {
        $sid = $null
        try { $sid = $entry.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value } catch {
            if ($entry.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow -and
                (($entry.FileSystemRights -band $writeRights) -ne 0)) {
                throw "Refusing terminal-isolation mutation because an unresolvable write-capable ACL applies to: $Path"
            }
            continue
        }
        if ($entry.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow -and
            (($null -eq $sid) -or $sid -notin $TrustedOwnerSids) -and
            (($entry.FileSystemRights -band $writeRights) -ne 0)) {
            throw "Refusing terminal-isolation mutation because a user-writable ACL applies to: $Path"
        }
    }
}

function Assert-StateOwnedPath([string] $Path, [bool] $AllowMissingLeaf = $false) {
    $full = Get-FullPath $Path
    $root = $ExpectedStateRoot.TrimEnd('\') + '\'
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($full, $ExpectedStateRoot) -and
        -not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing a path outside the owned terminal-isolation root: $Path"
    }
    Assert-NoReparsePath $full $AllowMissingLeaf
    if (Test-Path -LiteralPath $full) { Assert-TrustedOwner $full }
}

function Protect-Path([string] $Path, [bool] $Directory = $false) {
    Assert-StateOwnedPath $Path
    $grant = if ($Directory) { '*S-1-5-18:(OI)(CI)(F)', '*S-1-5-32-544:(OI)(CI)(F)' } else { '*S-1-5-18:(F)', '*S-1-5-32-544:(F)' }
    & icacls.exe $Path /inheritance:r /grant:r $grant[0] $grant[1] | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not protect terminal-isolation state: $Path" }
    Assert-StateOwnedPath $Path
}

function New-ProtectedProductDirectory([string] $Path) {
    # Directory.CreateDirectory(path, security) applies the non-inherited ACL
    # as part of creation.  If another actor wins the create race, the call
    # returns the existing directory and we only validate it; we never seize or
    # rewrite a foreign pre-existing ProgramData\Vibeshine root.
    $security = New-Object System.Security.AccessControl.DirectorySecurity
    $security.SetAccessRuleProtection($true, $false)
    $security.SetOwner((New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')))
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            (New-Object System.Security.Principal.SecurityIdentifier($sid)),
            ([System.Security.AccessControl.FileSystemRights]::FullControl),
            ([System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [System.Security.AccessControl.InheritanceFlags]::ObjectInherit),
            ([System.Security.AccessControl.PropagationFlags]::None),
            ([System.Security.AccessControl.AccessControlType]::Allow))
        $security.AddAccessRule($rule)
    }
    [IO.Directory]::CreateDirectory($Path, $security) | Out-Null
}

function Ensure-StateRoot {
    $programData = Get-FullPath $env:ProgramData
    Assert-NoReparsePath $programData
    $vibeshineRoot = Join-Path $programData 'Vibeshine'
    if (-not (Test-Path -LiteralPath $vibeshineRoot -PathType Container)) {
        New-ProtectedProductDirectory $vibeshineRoot
        Assert-NoReparsePath $vibeshineRoot
        Assert-TrustedOwner $vibeshineRoot
    } else {
        # Existing roots are immutable from this feature's perspective.
        Assert-NoReparsePath $vibeshineRoot
        Assert-TrustedOwner $vibeshineRoot
    }
    if (-not (Test-Path -LiteralPath $StateRoot -PathType Container)) {
        New-ProtectedProductDirectory $StateRoot
        Assert-StateOwnedPath $StateRoot
    } else {
        Assert-StateOwnedPath $StateRoot
    }
}

function Set-Status([ValidateSet('active', 'pending-restart', 'pending-native-restart', 'unavailable', 'foreign-unavailable', 'rolled-back')] [string] $Status) {
    Ensure-StateRoot
    # Re-read both authoritative stores immediately before publishing a
    # terminal status; a stale observation must never be recorded as success.
    $null = Get-ServiceDllState
    $statePath = Join-Path $StateRoot $StateFileName
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        Assert-StateOwnedPath $statePath
        $null = Get-Content -LiteralPath $statePath -Raw
    }
    $path = Join-Path $StateRoot $StatusFileName
    if (Test-Path -LiteralPath $path) { Assert-StateOwnedPath $path }
    $temporary = Join-Path $StateRoot ('.status.' + [Guid]::NewGuid().ToString('N'))
    Set-Content -LiteralPath $temporary -Value $Status -Encoding ASCII -NoNewline
    Protect-Path $temporary
    Move-Item -LiteralPath $temporary -Destination $path -Force
    Assert-StateOwnedPath $path
}

function Get-Status {
    Ensure-StateRoot
    $path = Join-Path $StateRoot $StatusFileName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return '' }
    Assert-StateOwnedPath $path
    $status = Get-Content -LiteralPath $path -Raw
    return ([string]$status).Trim().ToLowerInvariant()
}

function Get-ServiceParameters([bool] $Writable = $false) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Default)
    $key = $base.OpenSubKey($ServiceParametersPath, $Writable)
    if ($null -eq $key) {
        if ($Writable) { $key = $base.CreateSubKey($ServiceParametersPath) }
        else { $base.Dispose(); throw 'TermService Parameters registry key is unavailable.' }
    }
    return @{ Base = $base; Key = $key }
}

function Get-ServiceDllState {
    $handles = Get-ServiceParameters
    try {
        $key = $handles.Key
        if ($key.GetValueNames() -notcontains 'ServiceDll') { return [pscustomobject]@{ Present = $false; Kind = ''; Value = '' } }
        $kind = $key.GetValueKind('ServiceDll')
        $value = $key.GetValue('ServiceDll', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if ($kind -notin @([Microsoft.Win32.RegistryValueKind]::String, [Microsoft.Win32.RegistryValueKind]::ExpandString)) {
            throw "TermService ServiceDll has unsupported registry type: $kind"
        }
        return [pscustomobject]@{ Present = $true; Kind = $kind.ToString(); Value = [string] $value }
    } finally { $handles.Key.Dispose(); $handles.Base.Dispose() }
}

function Set-ServiceDllState($State) {
    $handles = Get-ServiceParameters $true
    try {
        if (-not $State.Present) { $handles.Key.DeleteValue('ServiceDll', $false); return }
        $kind = [Enum]::Parse([Microsoft.Win32.RegistryValueKind], [string] $State.Kind)
        $handles.Key.SetValue('ServiceDll', [string] $State.Value, $kind)
    } finally { $handles.Key.Dispose(); $handles.Base.Dispose() }
}

function Expand-Path([string] $Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
    return Get-FullPath ([Environment]::ExpandEnvironmentVariables($Value.Trim('"')))
}

function Get-NativeServiceDll {
    return Get-FullPath (Join-Path ([Environment]::ExpandEnvironmentVariables('%SystemRoot%')) 'System32\termsrv.dll')
}

function Assert-NativeTermsrvIdentity {
    $path = Get-NativeServiceDll
    Assert-NoReparsePath $path
    Assert-TrustedOwner $path
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Terminal isolation requires the pinned x64 System32 termsrv.dll host.' }
    $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
    $version = [string] $item.VersionInfo.FileVersion
    if ($version -ne $NativeTermsrvFileVersion -or (Get-FileHashLower $path) -ne $NativeTermsrvSha256) {
        throw "Unsupported Microsoft termsrv.dll identity; expected the pinned $NativeTermsrvArchitecture build."
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch 'Microsoft') {
        throw 'The native termsrv.dll is not verifiably Microsoft-signed.'
    }
}

function Test-NativeServiceDll($State) {
    return $State.Present -and $State.Kind -eq [Microsoft.Win32.RegistryValueKind]::ExpandString.ToString() -and
        [StringComparer]::OrdinalIgnoreCase.Equals((Expand-Path $State.Value), (Get-NativeServiceDll))
}

function Test-PathEqual([string] $Left, [string] $Right) {
    if ([string]::IsNullOrWhiteSpace($Left) -or [string]::IsNullOrWhiteSpace($Right)) { return $false }
    return [StringComparer]::OrdinalIgnoreCase.Equals((Expand-Path $Left), (Expand-Path $Right))
}

function Read-State {
    Ensure-StateRoot
    $path = Join-Path $StateRoot $StateFileName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    Assert-StateOwnedPath $path
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

function Write-State($State) {
    Ensure-StateRoot
    $path = Join-Path $StateRoot $StateFileName
    if (Test-Path -LiteralPath $path) { Assert-StateOwnedPath $path }
    $temporary = Join-Path $StateRoot ('.state.' + [Guid]::NewGuid().ToString('N'))
    $State | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Protect-Path $temporary
    Move-Item -LiteralPath $temporary -Destination $path -Force
    Assert-StateOwnedPath $path
}

function Get-PackageRoot {
    if ([string]::IsNullOrWhiteSpace($InstallRoot)) { throw 'InstallRoot is required for package activation.' }
    $root = Get-FullPath (Join-Path $InstallRoot 'terminal-isolation')
    $programFiles = Get-FullPath $env:ProgramFiles
    $programFilesPrefix = $programFiles.TrimEnd('\') + '\'
    $installFull = Get-FullPath $InstallRoot
    if (-not $installFull.StartsWith($programFilesPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Terminal isolation is restricted to a protected Program Files installation root.'
    }
    Assert-NoReparsePath $installFull
    Assert-TrustedOwner $installFull
    Assert-NoReparsePath $root
    Assert-TrustedOwner $root
    if (-not (Test-Path -LiteralPath (Join-Path $root 'terminal-isolation-manifest.json') -PathType Leaf)) {
        throw "Terminal-isolation package directory is missing: $root"
    }
    return $root
}

function Get-FileHashLower([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Verify-Package([string] $PackageRoot) {
    $manifestPath = Join-Path $PackageRoot 'terminal-isolation-manifest.json'
    Assert-NoReparsePath $manifestPath
    Assert-TrustedOwner $manifestPath
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 2 -or $manifest.provider -ne 'native-rdp-tcp') { throw 'Terminal-isolation manifest contract is invalid.' }
    $expected = @('TermWrap.dll', 'Zydis.dll', 'LICENSE', 'terminal-isolation.ps1', 'status-contract.txt')
    $actual = @($manifest.assets | ForEach-Object { [string] $_.path })
    if ($actual.Count -ne $expected.Count -or @($expected | Where-Object { $_ -notin $actual }).Count -ne 0) { throw 'Terminal-isolation manifest asset set is invalid.' }
    foreach ($asset in $manifest.assets) {
        if ([IO.Path]::GetFileName([string] $asset.path) -ne [string] $asset.path) { throw 'Manifest contains an unsafe asset path.' }
        $path = Join-Path $PackageRoot ([string] $asset.path)
        Assert-NoReparsePath $path
        Assert-TrustedOwner $path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Manifest asset is missing: $path" }
        $actualHash = Get-FileHashLower $path
        if (([string]$asset.path -in $RuntimePinnedHashes.Keys) -and $actualHash -ne $RuntimePinnedHashes[[string]$asset.path]) {
            throw "Runtime compatibility pin mismatch: $path"
        }
        if ($actualHash -ne ([string] $asset.sha256).ToLowerInvariant()) { throw "Manifest hash mismatch: $path" }
    }
    return $manifest
}

function Validate-State($State) {
    if ($null -eq $State -or $State.Schema -ne $StateSchema -or $State.Owner -ne $OwnedMarker -or $State.Provider -ne 'native-rdp-tcp') {
        throw 'Terminal-isolation state is missing, foreign, or an unsupported schema.'
    }
    if ([string]$State.ManifestSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Terminal-isolation state has an invalid manifest identity.' }
    foreach ($property in @('PayloadDirectory', 'OwnedServiceDll', 'HelperPath', 'ManifestPath')) {
        $value = [string] $State.$property
        if ([string]::IsNullOrWhiteSpace($value)) { throw "Terminal-isolation state field is missing: $property" }
        Assert-StateOwnedPath $value
    }
    if (-not (Test-Path -LiteralPath $State.PayloadDirectory -PathType Container) -or
        -not (Test-Path -LiteralPath $State.OwnedServiceDll -PathType Leaf) -or
        -not (Test-Path -LiteralPath $State.HelperPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $State.ManifestPath -PathType Leaf)) { throw 'Terminal-isolation immutable payload is incomplete.' }
    $expectedPayload = Join-Path $StateRoot "$PayloadDirectoryName\$([string]$State.ManifestSha256)"
    if (-not (Test-PathEqual $State.PayloadDirectory $expectedPayload)) { throw 'Terminal-isolation payload is not bound to the manifest identity.' }
    if (-not (Test-PathEqual $State.OwnedServiceDll (Join-Path $State.PayloadDirectory 'TermWrap.dll')) -or
        -not (Test-PathEqual $State.HelperPath (Join-Path $State.PayloadDirectory 'terminal-isolation.ps1')) -or
        -not (Test-PathEqual $State.ManifestPath (Join-Path $State.PayloadDirectory 'terminal-isolation-manifest.json'))) { throw 'Terminal-isolation state paths are not content-addressed.' }
    if ((Get-FileHashLower $State.ManifestPath) -ne ([string]$State.ManifestSha256).ToLowerInvariant()) { throw 'Terminal-isolation embedded manifest hash validation failed.' }
    $embeddedManifest = Get-Content -LiteralPath $State.ManifestPath -Raw | ConvertFrom-Json
    if ($embeddedManifest.schema -ne 2 -or $embeddedManifest.provider -ne 'native-rdp-tcp' -or @($embeddedManifest.assets).Count -ne 5) { throw 'Terminal-isolation embedded manifest contract is invalid.' }
    $stateAssets = @($State.PayloadFiles)
    if ($stateAssets.Count -ne 5) { throw 'Terminal-isolation state has an incomplete payload manifest.' }
    $expectedAssetNames = @('TermWrap.dll', 'Zydis.dll', 'LICENSE', 'terminal-isolation.ps1', 'status-contract.txt')
    $stateAssetNames = @($stateAssets | ForEach-Object { [string] $_.Name })
    if (@($expectedAssetNames | Where-Object { $_ -notin $stateAssetNames }).Count -ne 0 -or
        @($stateAssetNames | Where-Object { $_ -notin $expectedAssetNames }).Count -ne 0) {
        throw 'Terminal-isolation state has an invalid payload manifest.'
    }
    foreach ($asset in $stateAssets) {
        $name = [string] $asset.Name
        if ([IO.Path]::GetFileName($name) -ne $name) { throw 'Terminal-isolation state contains an unsafe payload name.' }
        $path = Join-Path $State.PayloadDirectory $name
        Assert-StateOwnedPath $path
        if ($RuntimePinnedHashes.ContainsKey($name) -and ([string]$asset.Sha256).ToLowerInvariant() -ne $RuntimePinnedHashes[$name]) { throw "Terminal-isolation runtime pin mismatch: $name" }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHashLower $path) -ne ([string] $asset.Sha256).ToLowerInvariant()) { throw "Terminal-isolation payload hash validation failed: $name" }
        $embeddedAsset = @($embeddedManifest.assets | Where-Object { [string]$_.path -eq $name })
        if ($embeddedAsset.Count -ne 1 -or ([string]$embeddedAsset[0].sha256).ToLowerInvariant() -ne ([string]$asset.Sha256).ToLowerInvariant()) { throw "Terminal-isolation embedded manifest does not bind: $name" }
    }
    if (-not (Test-NativeServiceDll $State.OriginalServiceDll)) { throw 'Terminal-isolation state has an unsafe original ServiceDll.' }
}

function Get-ClaimedOwnedServiceDll($State) {
    if ($null -eq $State -or $null -eq $State.PSObject.Properties['OwnedServiceDll']) { return '' }
    return [string]$State.OwnedServiceDll
}

function Test-ClaimedOwnedState($State) {
    return $null -ne $State -and $null -ne $State.PSObject.Properties['Owner'] -and
        [string]$State.Owner -eq $OwnedMarker
}

function Stage-Payload($Manifest, [string] $PackageRoot, [string] $ManifestHash) {
    Ensure-StateRoot
    $payloadBase = Join-Path $StateRoot $PayloadDirectoryName
    if (-not (Test-Path -LiteralPath $payloadBase -PathType Container)) {
        New-Item -ItemType Directory -Path $payloadBase -Force | Out-Null
        Assert-StateOwnedPath $payloadBase
        Protect-Path $payloadBase $true
    } else {
        Assert-StateOwnedPath $payloadBase
    }
    $payloadRoot = Join-Path $payloadBase $ManifestHash
    if (Test-Path -LiteralPath $payloadRoot) {
        Assert-StateOwnedPath $payloadRoot
        foreach ($asset in $Manifest.assets) {
            $path = Join-Path $payloadRoot ([string] $asset.path)
            Assert-StateOwnedPath $path
            if ((Get-FileHashLower $path) -ne ([string] $asset.sha256).ToLowerInvariant()) { throw 'An existing content-addressed payload has a hash collision or was modified.' }
        }
        $embeddedManifest = Join-Path $payloadRoot 'terminal-isolation-manifest.json'
        Assert-StateOwnedPath $embeddedManifest
        if ((Get-FileHashLower $embeddedManifest) -ne $ManifestHash) { throw 'An existing payload has an unbound embedded manifest.' }
        return $payloadRoot
    }
    $stage = Join-Path $StateRoot ('.payload.' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Assert-StateOwnedPath $stage
    Protect-Path $stage $true
    try {
        foreach ($asset in $Manifest.assets) {
            $source = Join-Path $PackageRoot ([string] $asset.path)
            $destination = Join-Path $stage ([string] $asset.path)
            Copy-Item -LiteralPath $source -Destination $destination
            Protect-Path $destination
            if ((Get-FileHashLower $destination) -ne ([string] $asset.sha256).ToLowerInvariant()) { throw "Staged payload hash validation failed: $destination" }
        }
        $manifestSource = Join-Path $PackageRoot 'terminal-isolation-manifest.json'
        $manifestDestination = Join-Path $stage 'terminal-isolation-manifest.json'
        Copy-Item -LiteralPath $manifestSource -Destination $manifestDestination
        Protect-Path $manifestDestination
        if ((Get-FileHashLower $manifestDestination) -ne $ManifestHash) { throw 'Staged embedded manifest hash validation failed.' }
        Move-Item -LiteralPath $stage -Destination $payloadRoot
        Assert-StateOwnedPath $payloadRoot
    } catch {
        # Preserve the protected staging directory for diagnosis.  The boot
        # helper must never recursively delete the tree it may execute from.
        throw
    }
    return $payloadRoot
}

function Register-VerificationTask {
    $state = Read-State
    Validate-State $state
    $null = Get-ServiceDllState
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $script = [string] $state.HelperPath
    $command = '"{0}" -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "{1}" -Action Verify -StateRoot "{2}"' -f $powershell, $script, ([IO.Path]::GetFullPath($StateRoot))
    & schtasks.exe /Create /TN $TaskName /SC ONSTART /DELAY 0000:30 /RU SYSTEM /RL HIGHEST /TR $command /F | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not register the SYSTEM terminal-isolation verification task.' }
}

function Remove-VerificationTask {
    try {
        $null = Get-ServiceDllState
        $null = Read-State
        & schtasks.exe /Delete /TN $TaskName /F | Out-Null
    } catch {}
}

function Test-TermServiceRunning {
    $service = Get-CimInstance Win32_Service -Filter "Name='TermService'"
    return $null -ne $service -and $service.State -eq 'Running'
}

function Test-TermServiceLoaded($State) {
    try {
        $service = Get-CimInstance Win32_Service -Filter "Name='TermService'"
        if ($null -eq $service -or $service.State -ne 'Running' -or $service.ProcessId -eq 0) { return $false }
        $modules = Get-Process -Id $service.ProcessId -Module -ErrorAction Stop
        $native = Get-NativeServiceDll
        $wrap = @($modules | Where-Object { $_.ModuleName -ieq 'TermWrap.dll' -and (Test-PathEqual $_.FileName $State.OwnedServiceDll) }).Count -gt 0
        $termsrv = @($modules | Where-Object { $_.ModuleName -ieq 'termsrv.dll' -and (Test-PathEqual $_.FileName $native) }).Count -gt 0
        return $wrap -and $termsrv -and (Get-FileHashLower $State.OwnedServiceDll) -eq ((@($State.PayloadFiles | Where-Object { $_.Name -ieq 'TermWrap.dll' })[0]).Sha256.ToLowerInvariant())
    } catch { return $false }
}

function Test-NativeTermServiceLoaded {
    try {
        $service = Get-CimInstance Win32_Service -Filter "Name='TermService'"
        if ($null -eq $service -or $service.State -ne 'Running' -or $service.ProcessId -eq 0) { return $false }
        $modules = Get-Process -Id $service.ProcessId -Module -ErrorAction Stop
        $native = Get-NativeServiceDll
        return @($modules | Where-Object { $_.ModuleName -ieq 'termsrv.dll' -and (Test-PathEqual $_.FileName $native) }).Count -gt 0 -and
            @($modules | Where-Object { $_.ModuleName -ieq 'TermWrap.dll' }).Count -eq 0
    } catch { return $false }
}

function Invoke-Install {
    $packageRoot = Get-PackageRoot
    $manifest = Verify-Package $packageRoot
    Assert-NativeTermsrvIdentity
    $manifestHash = Get-FileHashLower (Join-Path $packageRoot 'terminal-isolation-manifest.json')
    $state = Read-State
    $current = Get-ServiceDllState
    if ($null -ne $state) {
        if (-not (Test-ClaimedOwnedState $state)) { Fail 'Refusing terminal isolation: existing ProgramData state belongs to another provider.' }
        Validate-State $state
        if ([string] $state.ManifestSha256 -ne $manifestHash) {
            # A major-upgrade rollback cannot yet restore the complete prior
            # state and registry binding transactionally.  Refuse a manifest
            # transition rather than leaving the old product with new state.
            Fail 'Manifest-changing terminal-isolation upgrades are blocked until transactional prior-state restoration is implemented.'
        }
        if (-not (Test-NativeServiceDll $current) -and -not (Test-PathEqual $current.Value $state.OwnedServiceDll)) { Fail 'TermService ServiceDll was changed by another provider.' }
    } elseif (-not (Test-NativeServiceDll $current)) {
        Fail 'Refusing terminal isolation: TermService ServiceDll is not the native inbox termsrv.dll.'
    }
    $manifestChanged = $false
    $payload = if ($null -ne $state) { [string] $state.PayloadDirectory } else { Stage-Payload $manifest $packageRoot $manifestHash }
    $files = @($manifest.assets | ForEach-Object { [pscustomobject]@{ Name = [string] $_.path; Sha256 = ([string] $_.sha256).ToLowerInvariant() } })
    $original = if ($null -ne $state) { $state.OriginalServiceDll } else { $current }
    if ($null -ne $state -and -not $manifestChanged) {
        # The state is already complete and immutable.  Do not rewrite it
        # before changing the registry; this keeps rollback bound to the
        # known-good payload.
        Set-ServiceDllState ([pscustomobject]@{ Present = $true; Kind = 'ExpandString'; Value = [string] $state.OwnedServiceDll })
        $installed = Get-ServiceDllState
        if (-not (Test-PathEqual $installed.Value $state.OwnedServiceDll)) { Fail 'Existing terminal-isolation ServiceDll could not be re-armed exactly.' }
        Register-VerificationTask
        Set-Status 'pending-restart'
        return
    }
    $previousPayloads = @()
    if ($null -ne $state) {
        if ($null -ne $state.PSObject.Properties['PreviousPayloadDirectories']) {
            $previousPayloads += @($state.PreviousPayloadDirectories)
        }
        $previousPayloads += [string] $state.PayloadDirectory
    }
    $newState = [pscustomobject]@{
        Schema = $StateSchema; Owner = $OwnedMarker; Provider = 'native-rdp-tcp'; ManifestSha256 = $manifestHash
        OriginalServiceDll = $original; OwnedServiceDll = (Join-Path $payload 'TermWrap.dll'); PayloadDirectory = $payload
        ManifestPath = (Join-Path $payload 'terminal-isolation-manifest.json'); PayloadFiles = $files
        HelperPath = (Join-Path $payload 'terminal-isolation.ps1'); TaskName = $TaskName
        PreviousPayloadDirectories = $previousPayloads
    }
    Write-State $newState
    Protect-Path $StateRoot $true
    Set-ServiceDllState ([pscustomobject]@{ Present = $true; Kind = 'ExpandString'; Value = [string] $newState.OwnedServiceDll })
    $installed = Get-ServiceDllState
    if ($installed.Kind -ne 'ExpandString' -or -not (Test-PathEqual $installed.Value $newState.OwnedServiceDll)) { Fail 'Terminal-isolation ServiceDll was not installed exactly.' }
    Register-VerificationTask
    Set-Status 'pending-restart'
}

function Invoke-Rollback {
    $state = Read-State
    if ($null -eq $state) { return }
    if (-not (Test-ClaimedOwnedState $state)) { return }
    $current = Get-ServiceDllState
    $claimedOwnedServiceDll = Get-ClaimedOwnedServiceDll $state
    if ($current.Present -and -not (Test-NativeServiceDll $current) -and
        ([string]::IsNullOrWhiteSpace($claimedOwnedServiceDll) -or -not (Test-PathEqual $current.Value $claimedOwnedServiceDll))) {
        # Do not let malformed/foreign state turn an unrelated provider into
        # an MSI failure, and never overwrite its ServiceDll.
        Set-Status 'foreign-unavailable'
        return
    }
    Validate-State $state
    if ($current.Present -and (Test-PathEqual $current.Value $state.OwnedServiceDll)) {
        if (-not (Test-NativeServiceDll $state.OriginalServiceDll)) { Fail 'Refusing rollback: the saved ServiceDll is not the native inbox termsrv.dll.' }
        Set-ServiceDllState $state.OriginalServiceDll
    } elseif ($current.Present -and -not (Test-NativeServiceDll $current)) {
        # Never overwrite a foreign provider. Preserve the owned payload and
        # let MSI uninstall/replacement continue with an explicit diagnostic.
        Set-Status 'foreign-unavailable'
        return
    }
    if (Test-PathEqual (Get-ServiceDllState).Value $state.OwnedServiceDll) { Fail 'Rollback left TermService pointing at Vibeshine wrapper files.' }
    $afterRollback = Get-ServiceDllState
    if (Test-NativeServiceDll $afterRollback -and (Test-NativeTermServiceLoaded)) {
        # The native DLL is already what TermService has loaded.  Do not arm a
        # task that can overwrite this settled state on the next boot; publish
        # the verified terminal state and make task deletion best-effort.
        Set-Status 'rolled-back'
        Remove-VerificationTask
        return
    }
    Register-VerificationTask
    Set-Status 'pending-native-restart'
}

function Invoke-Rearm {
    $state = Read-State
    if ($null -eq $state -or -not (Test-ClaimedOwnedState $state)) { return }
    Validate-State $state
    Assert-NativeTermsrvIdentity
    $current = Get-ServiceDllState
    if ($current.Present -and -not (Test-NativeServiceDll $current) -and
        -not (Test-PathEqual $current.Value $state.OwnedServiceDll)) {
        Set-Status 'foreign-unavailable'
        return
    }
    Set-ServiceDllState ([pscustomobject]@{ Present = $true; Kind = 'ExpandString'; Value = [string] $state.OwnedServiceDll })
    $installed = Get-ServiceDllState
    if ($installed.Kind -ne 'ExpandString' -or -not (Test-PathEqual $installed.Value $state.OwnedServiceDll)) {
        Fail 'Terminal-isolation compensation could not re-arm the validated wrapper.'
    }
    Register-VerificationTask
    Set-Status 'pending-restart'
}

function Invoke-Verify {
    $state = Read-State
    if ($null -eq $state) { return }
    if (-not (Test-ClaimedOwnedState $state)) { return }
    $current = Get-ServiceDllState
    $claimedOwnedServiceDll = Get-ClaimedOwnedServiceDll $state
    if ($current.Present -and -not (Test-NativeServiceDll $current) -and
        ([string]::IsNullOrWhiteSpace($claimedOwnedServiceDll) -or -not (Test-PathEqual $current.Value $claimedOwnedServiceDll))) {
        Set-Status 'foreign-unavailable'
        return
    }
    Validate-State $state
    if (Test-NativeServiceDll $current) {
        if (-not (Test-TermServiceRunning) -or -not (Test-NativeTermServiceLoaded)) { Set-Status 'pending-native-restart'; return }
        # Keep the immutable state/payload as protected forensic evidence.  A
        # later explicit opt-in can safely reuse or add another content hash.
        Set-Status 'rolled-back'
        Remove-VerificationTask
        return
    }
    if (Test-PathEqual $current.Value $state.OwnedServiceDll) {
        if (Test-TermServiceLoaded $state) { Set-Status 'active'; Remove-VerificationTask; return }
        Invoke-Rollback
        return
    }
    Set-Status 'foreign-unavailable'
}

$terminalIsolationLock = $null
$terminalIsolationLockHeld = $false
try {
    Ensure-StateRoot
    $lockPath = Join-Path $StateRoot '.action.lock'
    if (Test-Path -LiteralPath $lockPath) {
        Assert-StateOwnedPath $lockPath
    } else {
        New-Item -ItemType File -Path $lockPath -Force | Out-Null
        Protect-Path $lockPath
    }
    for ($attempt = 0; $attempt -lt 30 -and $null -eq $terminalIsolationLock; $attempt++) {
        try {
            $terminalIsolationLock = [IO.File]::Open($lockPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $terminalIsolationLockHeld = $true
        } catch [IO.IOException] {
            Start-Sleep -Milliseconds 1000
        }
    }
    if (-not $terminalIsolationLockHeld) {
        if ($Action -eq 'Verify') { exit 0 }
        throw 'Another terminal-isolation action is still running; retry the MSI operation.'
    }
    switch ($Action) {
        'Install' { Invoke-Install }
        'Rollback' { Invoke-Rollback }
        'Rearm' { Invoke-Rearm }
        'Verify' { Invoke-Verify }
    }
    exit 0
} catch {
    if ($terminalIsolationLockHeld) { try { Set-Status 'unavailable' } catch {} }
    Write-Error $_
    exit 1
} finally {
    if ($null -ne $terminalIsolationLock) { $terminalIsolationLock.Dispose() }
}
