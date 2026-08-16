[CmdletBinding()]
param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidateSet('x64','ARM64')][string]$Platform = 'x64',
    [string]$MsBuildPath,
    [string]$WdkVersion = '10.0.26100.0',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'SteamOfflineFilter.vcxproj'
if (-not (Test-Path -LiteralPath $project -PathType Leaf)) { throw "Project not found: $project" }
if ([string]::IsNullOrWhiteSpace($MsBuildPath)) {
    $MsBuildPath = (Get-Command msbuild.exe -ErrorAction SilentlyContinue).Source
}
if ([string]::IsNullOrWhiteSpace($MsBuildPath)) {
    throw 'MSBuild with the Windows Driver Kit is required; no build is attempted.'
}
$wdkRoots = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'),
    (Join-Path ${env:ProgramFiles} 'Windows Kits\10')
)
if ($env:WindowsSdkDir) { $wdkRoots += $env:WindowsSdkDir.TrimEnd('\') }
$wdkTarget = $wdkRoots |
    ForEach-Object { Join-Path $_ "build\$WdkVersion\WindowsDriver.Common.props" } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $wdkTarget) {
    throw "WDK $WdkVersion was not found under the installed Windows Kits roots: $($wdkRoots -join ', '); no build is attempted."
}
$output = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $PSScriptRoot 'build'
} else {
    [IO.Path]::GetFullPath($OutputDirectory)
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$msbuildConfiguration = if ($Configuration -eq 'RelWithDebInfo') { 'Release' } else { $Configuration }
& $MsBuildPath $project /m "/p:Configuration=$msbuildConfiguration" "/p:Platform=$Platform" "/p:TargetVersion=$WdkVersion" "/p:OutDir=$output\"
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }
