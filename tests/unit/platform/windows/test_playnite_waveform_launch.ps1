param([switch]$CompileBridge, [string]$NativeHelperDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$module = Join-Path $repo 'plugins/playnite/SunshinePlaynite/SunshinePlaynite.psm1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($module, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
function Read-Function([string]$name) {
  $node = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true)
  if (-not $node) { throw "Missing production function: $name" }
  return $node.Extent.Text
}
if ($CompileBridge) {
  Invoke-Expression (Read-Function 'Initialize-WaveformLaunchBridge')
  Initialize-WaveformLaunchBridge
  if ([SunshineWaveformLaunchBridgeV1]::Prepare('', 'mapping', $repo, $false)) { throw 'Empty library path accepted' }
  if ($NativeHelperDirectory) {
    # A missing mapping must fail inside the 64-bit helper. In a 32-bit host,
    # loading the DLL locally would instead fail with ERROR_BAD_EXE_FORMAT.
    $missingMapping = 'Local\Vibeshine.Missing.Mapping.' + [Guid]::NewGuid().ToString('N')
    $dll = Join-Path $NativeHelperDirectory 'vibeshine_dualsense_audio.dll'
    $failure = $null
    try { [void][SunshineWaveformLaunchBridgeV1]::Prepare($dll, $missingMapping, $repo, $true) }
    catch { $failure = $_.Exception.ToString() }
    if (-not $failure -or $failure -notmatch 'Steam hook preparation failed') {
      throw "Storefront setup did not reach the native helper: $failure"
    }
    "Native helper reached from $([IntPtr]::Size * 8)-bit connector host"
  }
  'Native Playnite bridge compiles and rejects missing libraries'
  exit
}
Add-Type @'
public static class SunshineWaveformLaunchBridgeV1 {
  public static int Calls;
  public static bool Result = true;
  public static string Mapping, Directory;
  public static bool Steam;
  public static bool Prepare(string dll, string mapping, string directory, bool steam) {
    Calls++; Mapping = mapping; Directory = directory; Steam = steam; return Result;
  }
}
'@
Invoke-Expression (Read-Function 'Initialize-WaveformLaunchBridge')
Invoke-Expression (Read-Function 'OnGameStarting')
function Write-Log { param($Message) }
$script:Stops = 0
function Send-StatusMessage {
  param($Name, $Game)
  if ($Name -ne 'waveformSetupFailed') { throw 'Unexpected cancellation status' }
  $script:Stops++
}
function Assert($condition, $message) { if (-not $condition) { throw $message } }
$event = [pscustomobject]@{
  Game = [pscustomobject]@{ Name = 'Test'; PluginId = 'cb91dfc9-b977-43bf-8e70-55f46e410fab'; InstallDirectory = 'C:\Games\Test' }
  CancelStartup = $false
}
$previousMapping = $env:VIBESHINE_DUALSENSE_HAPTICS_MAPPING
$previousDll = $env:VIBESHINE_DUALSENSE_HAPTICS_DLL
try {
  $env:VIBESHINE_DUALSENSE_HAPTICS_MAPPING = ''
  OnGameStarting $event
  Assert ([SunshineWaveformLaunchBridgeV1]::Calls -eq 0) 'Ordinary launches must not load hooks'
  $env:VIBESHINE_DUALSENSE_HAPTICS_MAPPING = 'first-session'
  $env:VIBESHINE_DUALSENSE_HAPTICS_DLL = 'C:\Host\tools\vibeshine_dualsense_audio.dll'
  OnGameStarting $event
  Assert ([SunshineWaveformLaunchBridgeV1]::Steam) 'Steam handoff was not prepared'
  Assert ([SunshineWaveformLaunchBridgeV1]::Directory -eq $event.Game.InstallDirectory) 'Wrong game scope'
  Assert (-not $event.CancelStartup) 'Prepared launch was cancelled'
  $env:VIBESHINE_DUALSENSE_HAPTICS_MAPPING = 'second-session'
  $event.Game.PluginId = [Guid]::Empty
  OnGameStarting $event
  Assert ([SunshineWaveformLaunchBridgeV1]::Mapping -eq 'second-session') 'Reconnection reused the old mapping'
  Assert (-not [SunshineWaveformLaunchBridgeV1]::Steam) 'Direct Playnite game was treated as Steam'
  [SunshineWaveformLaunchBridgeV1]::Result = $false
  OnGameStarting $event
  Assert $event.CancelStartup 'Game was launched without its requested hook'
  Assert ($script:Stops -eq 1) 'Launcher was not notified of failed setup'
  'Playnite prelaunch routing, reconnect, and cancellation checks passed'
} finally {
  $env:VIBESHINE_DUALSENSE_HAPTICS_MAPPING = $previousMapping
  $env:VIBESHINE_DUALSENSE_HAPTICS_DLL = $previousDll
}
