$ErrorActionPreference = 'Stop'

function Add-VddNativePathType {
    if ('SunshineVddNativePath' -as [type]) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class SunshineVddNativePath {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string name, uint access, uint share, IntPtr security, uint disposition,
        uint flags, IntPtr template);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(
        SafeFileHandle handle, [Out] char[] path, uint length, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool MoveFileExW(string existingName, string newName, uint flags);

    [StructLayout(LayoutKind.Sequential)]
    private struct CryptoBlob {
        public uint cbData;
        public IntPtr pbData;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AlgorithmIdentifier {
        public IntPtr pszObjId;
        public CryptoBlob Parameters;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SipIndirectData {
        public AlgorithmIdentifier Data;
        public AlgorithmIdentifier DigestAlgorithm;
        public CryptoBlob Digest;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CatalogMember {
        public uint cbStruct;
        public IntPtr pwszReferenceTag;
        public IntPtr pwszFileName;
        public Guid gSubjectType;
        public uint fdwMemberFlags;
        public IntPtr pIndirectData;
        public uint dwCertVersion;
        public uint dwReserved;
        public IntPtr hReserved;
        public CryptoBlob sEncodedIndirectData;
        public CryptoBlob sEncodedMemberInfo;
    }

    [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CryptCATOpen(string fileName, uint openFlags,
        IntPtr provider, uint publicVersion, uint encodingType);

    [DllImport("wintrust.dll", SetLastError = true)]
    private static extern bool CryptCATClose(IntPtr catalog);

    [DllImport("wintrust.dll", SetLastError = true)]
    private static extern IntPtr CryptCATEnumerateMember(IntPtr catalog, IntPtr previous);

    [DllImport("wintrust.dll", SetLastError = true)]
    private static extern bool CryptCATAdminCalcHashFromFileHandle(
        SafeFileHandle file, ref uint hashSize, IntPtr hash, uint flags);

    private static byte[] ReadBlob(CryptoBlob blob) {
        if (blob.cbData == 0 || blob.pbData == IntPtr.Zero) return Array.Empty<byte>();
        var bytes = new byte[blob.cbData];
        Marshal.Copy(blob.pbData, bytes, 0, bytes.Length);
        return bytes;
    }

    private static byte[] CatalogFileHash(string filePath) {
        const uint GENERIC_READ = 0x80000000;
        const uint FILE_SHARE_ALL = 0x7;
        const uint OPEN_EXISTING = 3;
        using (var file = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_ALL,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero)) {
            if (file.IsInvalid) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "CreateFileW failed while hashing a VDD catalog member.");
            }
            uint hashSize = 0;
            var sized = CryptCATAdminCalcHashFromFileHandle(file, ref hashSize, IntPtr.Zero, 0);
            if (!sized && Marshal.GetLastWin32Error() != 122 /* ERROR_INSUFFICIENT_BUFFER */) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "CryptCATAdminCalcHashFromFileHandle did not return a hash size.");
            }
            if (hashSize == 0) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "CryptCATAdminCalcHashFromFileHandle returned an empty hash.");
            }
            var hash = new byte[hashSize];
            var handle = Marshal.AllocHGlobal(hash.Length);
            try {
                if (!CryptCATAdminCalcHashFromFileHandle(file, ref hashSize, handle, 0)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "CryptCATAdminCalcHashFromFileHandle failed.");
                }
                Marshal.Copy(handle, hash, 0, (int)hashSize);
                if (hash.Length != hashSize) Array.Resize(ref hash, (int)hashSize);
                return hash;
            } finally {
                Marshal.FreeHGlobal(handle);
            }
        }
    }

    // CRYPTCAT_OPEN_VERIFYSIGHASH verifies the catalog's signature hash, not
    // its certificate chain. Membership is checked against the member's
    // SIP_INDIRECT_DATA digest, so this remains valid for a self-issued CAT
    // without importing or trusting its certificate.
    public static bool CatalogContainsFile(string catalogPath, string filePath) {
        var targetHash = CatalogFileHash(filePath);
        const uint CRYPTCAT_OPEN_EXISTING = 0x00000004;
        const uint CRYPTCAT_OPEN_VERIFYSIGHASH = 0x10000000;
        const uint CRYPTCAT_VERSION_2 = 0x00000200;
        const uint PKCS_7_ASN_ENCODING = 0x00010000;
        const uint X509_ASN_ENCODING = 0x00000001;
        var catalog = CryptCATOpen(catalogPath,
            CRYPTCAT_OPEN_EXISTING | CRYPTCAT_OPEN_VERIFYSIGHASH,
            IntPtr.Zero, CRYPTCAT_VERSION_2, PKCS_7_ASN_ENCODING | X509_ASN_ENCODING);
        if (catalog == IntPtr.Zero || catalog == new IntPtr(-1)) {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "CryptCATOpen failed while validating a VDD catalog.");
        }
        try {
            var previous = IntPtr.Zero;
            while (true) {
                var memberPointer = CryptCATEnumerateMember(catalog, previous);
                if (memberPointer == IntPtr.Zero) break;
                previous = memberPointer;
                var member = Marshal.PtrToStructure<CatalogMember>(memberPointer);
                if (member.pIndirectData == IntPtr.Zero) continue;
                var indirect = Marshal.PtrToStructure<SipIndirectData>(member.pIndirectData);
                var digest = ReadBlob(indirect.Digest);
                if (digest.SequenceEqual(targetHash)) return true;
            }
            return false;
        } finally {
            CryptCATClose(catalog);
        }
    }

    public static string GetFinalPath(string path) {
        const uint FILE_READ_ATTRIBUTES = 0x80;
        const uint FILE_SHARE_ALL = 0x7;
        const uint OPEN_EXISTING = 3;
        const uint FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
        using (var handle = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL,
            IntPtr.Zero, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, IntPtr.Zero)) {
            if (handle.IsInvalid) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "CreateFileW failed while resolving a VDD path.");
            }
            var buffer = new char[32768];
            var length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Length, 0);
            if (length == 0 || length >= buffer.Length) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "GetFinalPathNameByHandleW failed while resolving a VDD path.");
            }
            return new string(buffer, 0, (int)length);
        }
    }

    public static void MoveDirectory(string source, string destination) {
        const uint MOVEFILE_WRITE_THROUGH = 0x8;
        if (!MoveFileExW(source, destination, MOVEFILE_WRITE_THROUGH)) {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "MoveFileExW failed while publishing a VDD directory.");
        }
    }
}
'@
}

function Get-VddFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Get-VddFinalPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    Add-VddNativePathType
    $final = [SunshineVddNativePath]::GetFinalPath((Get-VddFullPath $Path))
    if ($final.StartsWith('\\?\UNC\', [System.StringComparison]::OrdinalIgnoreCase)) {
        $final = '\\' + $final.Substring(8)
    } elseif ($final.StartsWith('\\?\', [System.StringComparison]::OrdinalIgnoreCase)) {
        $final = $final.Substring(4)
    }
    return $final.TrimEnd('\', '/')
}

function Assert-VddContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$RequireLeaf
    )

    $rootFull = Get-VddFullPath $Root
    $pathFull = Get-VddFullPath $Path
    $rootPrefix = $rootFull.TrimEnd('\', '/') + '\'
    if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($pathFull, $rootFull) -and
        -not $pathFull.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "VDD path escapes trusted root '$rootFull': $pathFull"
    }
    if ($RequireLeaf -and -not (Test-Path -LiteralPath $pathFull -PathType Leaf)) {
        throw "VDD path is not a file: $pathFull"
    }
    return $pathFull
}

function Assert-VddSafePathComponents {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $rootFull = Assert-VddContainedPath -Root $Root -Path $Root
    $pathFull = Assert-VddContainedPath -Root $rootFull -Path $Path
    if (Test-Path -LiteralPath $rootFull) {
        $rootItem = Get-Item -LiteralPath $rootFull -Force
        if (($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "VDD trusted root is a reparse point: $rootFull"
        }
        Assert-VddContainedPath -Root $rootFull -Path (Get-VddFinalPath $rootFull) | Out-Null
    }
    $relative = $pathFull.Substring($rootFull.Length).TrimStart('\', '/')
    $current = $rootFull
    $components = if ($relative) { @($relative -split '[\\/]') } else { @() }
    foreach ($component in $components) {
        $current = Join-Path $current $component
        if (-not (Test-Path -LiteralPath $current)) { break }
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "VDD path component is a reparse point: $current"
        }
        Assert-VddContainedPath -Root $rootFull -Path (Get-VddFinalPath $current) | Out-Null
    }
    return $pathFull
}

function Assert-VddNoReparseTree {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string]$TrustedRoot = $Root,
        [switch]$RequireConsistentOwner
    )

    $rootFull = Assert-VddContainedPath -Root $TrustedRoot -Path $Root
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container)) {
        throw "VDD directory missing: $rootFull"
    }

    $items = @(Get-Item -LiteralPath $rootFull -Force) + @(
        Get-ChildItem -LiteralPath $rootFull -Recurse -Force -ErrorAction Stop
    )
    $rootOwner = (Get-Acl -LiteralPath $rootFull).Owner
    if ([string]::IsNullOrWhiteSpace($rootOwner)) {
        throw "VDD path has no identifiable owner: $rootFull"
    }
    foreach ($item in $items) {
        Assert-VddContainedPath -Root $TrustedRoot -Path $item.FullName | Out-Null
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "VDD path contains a reparse point: $($item.FullName)"
        }
        $final = Get-VddFinalPath $item.FullName
        Assert-VddContainedPath -Root $TrustedRoot -Path $final | Out-Null
        if ($RequireConsistentOwner -and (Get-Acl -LiteralPath $item.FullName).Owner -ne $rootOwner) {
            throw "VDD path owner differs from its root: $($item.FullName)"
        }
    }
    return $rootFull
}

function Remove-VddVerifiedTree {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$TrustedRoot
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $full = Assert-VddContainedPath -Root $TrustedRoot -Path $Path
    if ([System.StringComparer]::OrdinalIgnoreCase.Equals($full, (Get-VddFullPath $TrustedRoot))) {
        throw 'Refusing to remove the trusted VDD root.'
    }
    Assert-VddNoReparseTree -Root $full -TrustedRoot $TrustedRoot | Out-Null
    Remove-Item -LiteralPath $full -Recurse -Force
}

function New-VddSiblingPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$TrustedRoot,
        [string]$Suffix = 'stage'
    )

    $parent = Split-Path -Parent (Get-VddFullPath $Path)
    Assert-VddSafePathComponents -Root $TrustedRoot -Path $parent | Out-Null
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    do {
        $candidate = Join-Path $parent (".$([System.IO.Path]::GetFileName($Path)).$Suffix-$([Guid]::NewGuid().ToString('N'))")
    } while (Test-Path -LiteralPath $candidate)
    return $candidate
}

function Move-VddDirectoryAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$TrustedRoot
    )

    $stageFull = Assert-VddNoReparseTree -Root $Stage -TrustedRoot $TrustedRoot -RequireConsistentOwner
    $destinationFull = Assert-VddContainedPath -Root $TrustedRoot -Path $Destination
    $backup = New-VddSiblingPath -Path $Destination -TrustedRoot $TrustedRoot -Suffix 'backup'
    $hadDestination = Test-Path -LiteralPath $destinationFull
    if ($hadDestination) {
        Assert-VddNoReparseTree -Root $destinationFull -TrustedRoot $TrustedRoot -RequireConsistentOwner | Out-Null
        [SunshineVddNativePath]::MoveDirectory($destinationFull, $backup)
    }
    $published = $false
    try {
        # MoveFileExW without REPLACE_EXISTING cannot move the staged directory
        # inside an interposed destination container. If a swap wins the race,
        # publication fails and the old package remains recoverable at backup.
        [SunshineVddNativePath]::MoveDirectory($stageFull, $destinationFull)
        $published = $true
        Assert-VddNoReparseTree -Root $destinationFull -TrustedRoot $TrustedRoot -RequireConsistentOwner | Out-Null
    } catch {
        if ($published -and (Test-Path -LiteralPath $destinationFull)) {
            Remove-VddVerifiedTree -Path $destinationFull -TrustedRoot $TrustedRoot
        }
        if ($hadDestination -and (Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $destinationFull)) {
            [SunshineVddNativePath]::MoveDirectory($backup, $destinationFull)
        }
        throw
    }
    if ($hadDestination -and (Test-Path -LiteralPath $backup)) {
        Remove-VddVerifiedTree -Path $backup -TrustedRoot $TrustedRoot
    }
}

function Copy-VddDirectoryAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$SourceTrustedRoot,
        [Parameter(Mandatory = $true)][string]$DestinationTrustedRoot,
        [string]$MarkerName
    )

    $sourceFull = Assert-VddSafePathComponents -Root $SourceTrustedRoot -Path $Source
    Assert-VddNoReparseTree -Root $sourceFull -TrustedRoot $SourceTrustedRoot -RequireConsistentOwner | Out-Null
    $stage = New-VddSiblingPath -Path $Destination -TrustedRoot $DestinationTrustedRoot -Suffix 'copy'
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    try {
        Get-ChildItem -LiteralPath $sourceFull -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $stage -Recurse -Force
        }
        if ($MarkerName) {
            New-Item -ItemType File -Path (Join-Path $stage $MarkerName) -Force | Out-Null
        }
        Assert-VddNoReparseTree -Root $stage -TrustedRoot $DestinationTrustedRoot -RequireConsistentOwner | Out-Null
        Move-VddDirectoryAtomically -Stage $stage -Destination $Destination -TrustedRoot $DestinationTrustedRoot
    } finally {
        if (Test-Path -LiteralPath $stage) {
            try { Remove-VddVerifiedTree -Path $stage -TrustedRoot $DestinationTrustedRoot } catch { }
        }
    }
}

function Assert-VddZipEntryNames {
    param([Parameter(Mandatory = $true)][string]$ArchivePath)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('/', '\')
            if ([System.IO.Path]::IsPathRooted($name) -or $name -match '^[A-Za-z]:') {
                throw "Release archive contains an absolute path: $($entry.FullName)"
            }
            $segments = @($name -split '\\')
            for ($index = 0; $index -lt $segments.Count; $index++) {
                $segment = $segments[$index]
                $isTrailingDirectoryMarker = ($index -eq ($segments.Count - 1) -and $segment -eq '')
                if ($segment -eq '..' -or ($segment -eq '' -and -not $isTrailingDirectoryMarker)) {
                    throw "Release archive contains an unsafe path: $($entry.FullName)"
                }
            }
        }
    } finally {
        $archive.Dispose()
    }
}

function Read-VddManifest {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$TrustedBuildRoot
    )

    $manifestFull = Assert-VddContainedPath -Root $TrustedBuildRoot -Path $ManifestPath -RequireLeaf
    $manifestItem = Get-Item -LiteralPath $manifestFull -Force
    if (($manifestItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "VDD manifest is a reparse point: $manifestFull"
    }
    Assert-VddSafePathComponents -Root $TrustedBuildRoot -Path $manifestFull | Out-Null
    $manifest = Get-Content -LiteralPath $manifestFull -Raw | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or [string]::IsNullOrWhiteSpace($manifest.repository) -or
        [string]::IsNullOrWhiteSpace($manifest.tag) -or [string]::IsNullOrWhiteSpace($manifest.commit)) {
        throw "VDD manifest is missing its pinned release identity: $manifestFull"
    }
    if ($manifest.asset.size -le 0 -or $manifest.asset.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "VDD manifest has invalid archive evidence: $manifestFull"
    }
    $files = @($manifest.files)
    if ($files.Count -ne 7) {
        throw "VDD manifest must contain exactly seven payload files: $manifestFull"
    }
    $seen = @{}
    foreach ($file in $files) {
        if ([string]::IsNullOrWhiteSpace($file.path) -or [string]::IsNullOrWhiteSpace($file.archive_path) -or
            $file.size -le 0 -or $file.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "VDD manifest has an invalid payload entry: $manifestFull"
        }
        $key = $file.path.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            throw "VDD manifest contains duplicate payload path '$($file.path)': $manifestFull"
        }
        $seen[$key] = $true
    }
    if ($manifest.catalog_signer_thumbprint -notmatch '^[0-9a-fA-F]{40}$') {
        throw "VDD manifest has an invalid catalog signer thumbprint: $manifestFull"
    }
    $sourceFiles = @($manifest.source_files)
    if ($sourceFiles.Count -ne 2) {
        throw "VDD manifest must contain exactly two source-owned file entries: $manifestFull"
    }
    $sourceSeen = @{}
    $sourceNames = @('install.ps1', 'nefconc.exe')
    foreach ($file in $sourceFiles) {
        $name = [string]$file.path
        if ([string]::IsNullOrWhiteSpace($name) -or
            [System.IO.Path]::IsPathRooted($name) -or $name -match '[\\/]' -or
            $name -notin $sourceNames -or $sourceSeen.ContainsKey($name.ToLowerInvariant()) -or
            $file.size -le 0 -or $file.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "VDD manifest has an invalid or duplicate source-owned file entry: $manifestFull"
        }
        $sourceSeen[$name.ToLowerInvariant()] = $true
    }
    return [PSCustomObject]@{ Path = $manifestFull; Data = $manifest }
}

function Assert-VddSourceOwnedFiles {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$SourcePackageRoot,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$SourceTrustedRoot,
        [Parameter(Mandatory = $true)][string]$PackageTrustedRoot
    )
    Assert-VddNoReparseTree -Root $SourcePackageRoot -TrustedRoot $SourceTrustedRoot -RequireConsistentOwner | Out-Null
    Assert-VddNoReparseTree -Root $PackageRoot -TrustedRoot $PackageTrustedRoot -RequireConsistentOwner | Out-Null
    foreach ($entry in @($Manifest.Data.source_files)) {
        $source = Assert-VddContainedPath -Root $SourcePackageRoot -Path (Join-Path $SourcePackageRoot $entry.path) -RequireLeaf
        $package = Assert-VddContainedPath -Root $PackageRoot -Path (Join-Path $PackageRoot $entry.path) -RequireLeaf
        foreach ($path in @($source, $package)) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.Length -ne [int64]$entry.size -or
                (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256.ToLowerInvariant()) {
                throw "VDD source-owned file does not match the pinned manifest: $path"
            }
        }
    }
}

function Assert-VddManifestIdentity {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Tag
    )
    if ($Manifest.Data.repository -ne $Repository -or $Manifest.Data.tag -ne $Tag) {
        throw "VDD manifest identity '$($Manifest.Data.repository)@$($Manifest.Data.tag)' does not match '$Repository@$Tag'."
    }
}

function Get-VddManifestEntry {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$Path
    )
    return @($Manifest.Data.files | Where-Object { $_.path -eq $Path } | Select-Object -First 1)[0]
}

function Get-VddPayloadPath {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [switch]$PrebuiltLayout
    )
    if (-not $PrebuiltLayout) {
        return ($Entry.path -replace '/', '\')
    }
    if ($Entry.path -eq 'virtualdisplay_probe.exe') {
        return 'tools\virtualdisplay_probe.exe'
    }
    if ($Entry.path.StartsWith('vulkan-layer/', [System.StringComparison]::OrdinalIgnoreCase)) {
        return ($Entry.path -replace '/', '\')
    }
    return "driver\$($Entry.path)"
}

function Assert-VddManifestPayload {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$TrustedRoot,
        [switch]$PrebuiltLayout,
        [switch]$VerifyCatalog
    )

    Assert-VddNoReparseTree -Root $PackageRoot -TrustedRoot $TrustedRoot -RequireConsistentOwner | Out-Null
    foreach ($entry in @($Manifest.Data.files)) {
        $relative = Get-VddPayloadPath -Entry $entry -PrebuiltLayout:$PrebuiltLayout
        $path = Assert-VddContainedPath -Root $PackageRoot -Path (Join-Path $PackageRoot $relative) -RequireLeaf
        $item = Get-Item -LiteralPath $path -Force
        if ($item.Length -ne [int64]$entry.size) {
            throw "VDD payload size mismatch for '$($entry.path)': $($item.Length) != $($entry.size)."
        }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $entry.sha256.ToLowerInvariant()) {
            throw "VDD payload SHA256 mismatch for '$($entry.path)': $hash != $($entry.sha256)."
        }
    }
    if ($VerifyCatalog) {
        $catalog = Join-Path $PackageRoot (Get-VddPayloadPath -Entry (Get-VddManifestEntry -Manifest $Manifest -Path 'SunshineVirtualDisplayDriver.cat') -PrebuiltLayout:$PrebuiltLayout)
        $dll = Join-Path $PackageRoot (Get-VddPayloadPath -Entry (Get-VddManifestEntry -Manifest $Manifest -Path 'SunshineVirtualDisplayDriver.dll') -PrebuiltLayout:$PrebuiltLayout)
        $inf = Join-Path $PackageRoot (Get-VddPayloadPath -Entry (Get-VddManifestEntry -Manifest $Manifest -Path 'SunshineVirtualDisplayDriver.inf') -PrebuiltLayout:$PrebuiltLayout)
        $signature = Get-AuthenticodeSignature -LiteralPath $catalog
        if (-not $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint -ne $Manifest.Data.catalog_signer_thumbprint) {
            throw "VDD catalog signer does not match the pinned thumbprint."
        }
        $certificatePath = Join-Path $PackageRoot (Get-VddPayloadPath -Entry (Get-VddManifestEntry -Manifest $Manifest -Path 'SunshineVirtualDisplayDriver.cer') -PrebuiltLayout:$PrebuiltLayout)
        $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
        if ($certificate.Thumbprint -ne $Manifest.Data.catalog_signer_thumbprint) {
            throw "Bundled VDD certificate thumbprint does not match the pinned catalog signer."
        }
        Add-VddNativePathType
        foreach ($member in @($dll, $inf)) {
            # CryptCATOpen with CRYPTCAT_OPEN_VERIFYSIGHASH validates the
            # catalog's signed content without asking the trust provider to
            # build a certificate chain. Compare the member SIP digest to the
            # target's CryptCATAdmin file digest; this is the catalog
            # membership check and works for the pinned self-issued release CAT.
            if (-not [SunshineVddNativePath]::CatalogContainsFile($catalog, $member)) {
                throw "VDD catalog membership validation failed for $member."
            }
        }
    }
}

function Write-VddReadyRecord {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$TrustedRoot,
        [Parameter(Mandatory = $true)][string]$SourcePackageRoot,
        [Parameter(Mandatory = $true)][string]$SourceTrustedRoot,
        [Parameter(Mandatory = $true)][string]$ReadyPath
    )

    Assert-VddManifestPayload -Manifest $Manifest -PackageRoot $PackageRoot -TrustedRoot $TrustedRoot -VerifyCatalog
    Assert-VddSourceOwnedFiles -Manifest $Manifest -SourcePackageRoot $SourcePackageRoot -PackageRoot $PackageRoot -SourceTrustedRoot $SourceTrustedRoot -PackageTrustedRoot $TrustedRoot
    Assert-VddSafePathComponents -Root $TrustedRoot -Path $ReadyPath | Out-Null
    $payload = [ordered]@{}
    foreach ($entry in @($Manifest.Data.files)) {
        $path = Join-Path $PackageRoot (Get-VddPayloadPath -Entry $entry)
        $payload[$entry.path] = [ordered]@{
            size = [int64](Get-Item -LiteralPath $path).Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $sourcePayload = [ordered]@{}
    foreach ($entry in @($Manifest.Data.source_files)) {
        $path = Join-Path $SourcePackageRoot $entry.path
        $sourcePayload[$entry.path] = [ordered]@{
            size = [int64](Get-Item -LiteralPath $path).Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $record = [ordered]@{
        schema_version = 1
        provenance = 'pinned_release'
        local_signing = $false
        repository = $Manifest.Data.repository
        tag = $Manifest.Data.tag
        commit = $Manifest.Data.commit
        asset_name = $Manifest.Data.asset.name
        asset_size = [int64]$Manifest.Data.asset.size
        asset_sha256 = $Manifest.Data.asset.sha256.ToLowerInvariant()
        manifest_sha256 = (Get-FileHash -LiteralPath $Manifest.Path -Algorithm SHA256).Hash.ToLowerInvariant()
        catalog_signer_thumbprint = $Manifest.Data.catalog_signer_thumbprint.ToUpperInvariant()
        payload = $payload
        source_payload = $sourcePayload
    }
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReadyPath -Encoding UTF8
}

function Assert-VddReadyRecord {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$TrustedRoot,
        [Parameter(Mandatory = $true)][string]$ReadyPath,
        [Parameter(Mandatory = $true)][string]$SourcePackageRoot,
        [Parameter(Mandatory = $true)][string]$SourceTrustedRoot,
        [switch]$RequirePinnedProvenance
    )

    $readyFull = Assert-VddContainedPath -Root $TrustedRoot -Path $ReadyPath -RequireLeaf
    Assert-VddSafePathComponents -Root $TrustedRoot -Path $readyFull | Out-Null
    $record = Get-Content -LiteralPath $readyFull -Raw | ConvertFrom-Json
    if ($record.schema_version -ne 1 -or $record.provenance -ne 'pinned_release' -or
        ($RequirePinnedProvenance -and $record.local_signing -ne $false)) {
        throw "VDD ready record does not describe a pinned release: $readyFull"
    }
    $manifestHash = (Get-FileHash -LiteralPath $Manifest.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    foreach ($property in @('repository', 'tag', 'commit', 'asset_name', 'asset_sha256', 'manifest_sha256')) {
        $expected = switch ($property) {
            repository { $Manifest.Data.repository }
            tag { $Manifest.Data.tag }
            commit { $Manifest.Data.commit }
            asset_name { $Manifest.Data.asset.name }
            asset_sha256 { $Manifest.Data.asset.sha256.ToLowerInvariant() }
            manifest_sha256 { $manifestHash }
        }
        if ([string]$record.$property -ne [string]$expected) {
            throw "VDD ready record '$property' does not match the pinned manifest."
        }
    }
    if ([int64]$record.asset_size -ne [int64]$Manifest.Data.asset.size -or
        $record.catalog_signer_thumbprint.ToUpperInvariant() -ne $Manifest.Data.catalog_signer_thumbprint.ToUpperInvariant()) {
        throw "VDD ready record archive or signer evidence does not match the pinned manifest."
    }
    Assert-VddManifestPayload -Manifest $Manifest -PackageRoot $PackageRoot -TrustedRoot $TrustedRoot -VerifyCatalog
    Assert-VddSourceOwnedFiles -Manifest $Manifest -SourcePackageRoot $SourcePackageRoot -PackageRoot $PackageRoot -SourceTrustedRoot $SourceTrustedRoot -PackageTrustedRoot $TrustedRoot
    foreach ($entry in @($Manifest.Data.files)) {
        $payload = $record.payload.($entry.path)
        if ($null -eq $payload) { throw "VDD ready record is missing '$($entry.path)'." }
        $path = Join-Path $PackageRoot (Get-VddPayloadPath -Entry $entry)
        if ([int64]$payload.size -ne (Get-Item -LiteralPath $path).Length -or
            $payload.sha256.ToLowerInvariant() -ne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw "VDD ready record payload does not match '$($entry.path)'."
        }
    }
    foreach ($entry in @($Manifest.Data.source_files)) {
        $source = $record.source_payload.($entry.path)
        if ($null -eq $source -or [int64]$source.size -ne [int64]$entry.size -or
            $source.sha256.ToLowerInvariant() -ne $entry.sha256.ToLowerInvariant()) {
            throw "VDD ready record source-owned file does not match '$($entry.path)'."
        }
    }
}

function Resolve-VddTool {
    param([Parameter(Mandatory = $true)][string]$Name)
    foreach ($root in @('D:\Software\WinSDK\bin', "${env:ProgramFiles(x86)}\Windows Kits\10\bin")) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($version in (Get-ChildItem -LiteralPath $root -Directory | Sort-Object Name -Descending)) {
            foreach ($arch in @('x64', 'x86')) {
                $candidate = Join-Path $version.FullName "$arch\$Name"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
            }
        }
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw "Unable to locate Windows SDK tool $Name."
}
