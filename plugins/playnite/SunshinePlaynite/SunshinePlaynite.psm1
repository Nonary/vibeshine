<#
  Sunshine Playnite Connector (PowerShell Script Extension)
  - Connects to Sunshine over named pipes (control + data via anonymous handshake)
  - Sends categories and game metadata as JSON messages
  - Reconnects on failure and runs in background while Playnite is open

  Requires: Playnite script extension context (access to $PlayniteApi)
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$global:SunConn = $null

# Logger state
$script:LogPath = $null
$script:HostLogger = $null

function Resolve-LogPath {
  try {
    if ($PSScriptRoot) {
      return (Join-Path $PSScriptRoot 'sunshine_playnite.log')
    }
  } catch {}
  # Last resort: temp folder
  return (Join-Path $env:TEMP 'sunshine_playnite.log')
}

function Initialize-Logging {
  try {
    if (-not $script:LogPath) { $script:LogPath = Resolve-LogPath }
    if (Get-Variable -Name __logger -Scope Global -ErrorAction SilentlyContinue) { $script:HostLogger = $__logger } else { $script:HostLogger = $null }
    # Basic log rotation (~1 MB)
    if (Test-Path $script:LogPath) {
      $len = (Get-Item $script:LogPath).Length
      if ($len -gt 1MB) {
        Remove-Item -Path $script:LogPath -ErrorAction SilentlyContinue
      }
    }
    $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
    $hdr = @(
      "[$ts] === Sunshine Playnite Connector starting ===",
      "Process=$PID User=$env:USERNAME Session=$([Environment]::UserInteractive) PSVersion=$($PSVersionTable.PSVersion)"
    ) -join [Environment]::NewLine
    try {
      [System.IO.File]::WriteAllText($script:LogPath, $hdr + [Environment]::NewLine, [System.Text.Encoding]::UTF8)
    } catch {
      # Fallback to Out-File if static write fails
      $hdr | Out-File -FilePath $script:LogPath -Encoding utf8
    }
    if ($script:HostLogger) { $script:HostLogger.Info("SunshinePlaynite: logging initialized at $script:LogPath") }
  } catch {}
}

function Write-Log {
  param([string]$Message)
  try {
    $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
    if ($script:LogPath) {
      try {
        [System.IO.File]::AppendAllText($script:LogPath, "[$ts] $Message" + [Environment]::NewLine, [System.Text.Encoding]::UTF8)
      } catch {
        Add-Content -Path $script:LogPath -Value "[$ts] $Message" -Encoding utf8
      }
    }
    if ($script:HostLogger) { $script:HostLogger.Info("SunshinePlaynite: $Message") }
  } catch {}
}

# Native interop for probing named pipe availability
try {
  if (-not ([System.Management.Automation.PSTypeName]'Win32.NativeMethods').Type) {
    Add-Type -Namespace Win32 -Name NativeMethods -MemberDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeMethods {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool WaitNamedPipe(string lpNamedPipeName, uint nTimeOut);
}
"@
    Write-Log "Loaded Win32.NativeMethods for WaitNamedPipe"
  }
} catch {
  Write-Log "Failed to load Win32.NativeMethods: $($_.Exception.Message)"
}

function Get-FullPipeName {
  param([string]$Name)
  if ($Name -like "\\\\.\\pipe\\*") { return $Name }
  return "\\\\.\\pipe\\$Name"
}

function Test-PipeAvailable {
  param([string]$Name, [int]$TimeoutMs = 500)
  try {
    $full = Get-FullPipeName -Name $Name
    if ([Win32.NativeMethods]::WaitNamedPipe($full, [uint32]$TimeoutMs)) {
      Write-Log "WaitNamedPipe succeeded for '$full' within ${TimeoutMs}ms"
      return $true
    } else {
      $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
      Write-Log "WaitNamedPipe timeout/fail for '$full' err=$err after ${TimeoutMs}ms"
      return $false
    }
  } catch {
    Write-Log "Test-PipeAvailable exception: $($_.Exception.Message)"
    return $false
  }
}

function Connect-SunshinePipe {
  param(
    [string]$PublicName = 'sunshine_playnite_connector',
    [int]$TimeoutMs = 5000
  )

  try {
    Write-Log "Connecting public pipe '$PublicName' (timeout=${TimeoutMs}ms)"
    if (([System.Management.Automation.PSTypeName]'Win32.NativeMethods').Type) {
      $probeDeadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
      while ([DateTime]::UtcNow -lt $probeDeadline) {
        if (Test-PipeAvailable -Name $PublicName -TimeoutMs 300) { break }
        Start-Sleep -Milliseconds 200
      }
    }
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PublicName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
    $pipe.Connect([Math]::Max(1000, [int]($TimeoutMs/2)))
    $writer = New-Object System.IO.StreamWriter($pipe, [System.Text.Encoding]::UTF8, 8192, $true)
    $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($pipe, [System.Text.Encoding]::UTF8, $false, 8192, $true)
    Write-Log "Public pipe connected: CanRead=$($pipe.CanRead) CanWrite=$($pipe.CanWrite) IsConnected=$($pipe.IsConnected)"
    return @{ Stream = $pipe; Writer = $writer; Reader = $reader }
  } catch {
    if ($pipe) { try { $pipe.Dispose() } catch {} }
    $ex = $_.Exception
    Write-Log "Public connect failed: $($ex.GetType().FullName): $($ex.Message) HResult=$([String]::Format('0x{0:X8}', $ex.HResult))"
    throw
  }
}

function Send-JsonMessage {
  param(
    [Parameter(Mandatory)] [string]$Json
  )
  if (-not $global:SunConn -or -not $global:SunConn.Stream -or -not $global:SunConn.Stream.CanWrite) {
    try { $global:SunConn = Connect-SunshinePipe -TimeoutMs 5000; Write-Log "Connected to Sunshine (send path)" } catch { Write-Log "Connect-SunshinePipe failed in Send-JsonMessage: $($_.Exception.Message)"; return }
  }
  try {
    if ($global:SunConn.Writer) {
      $global:SunConn.Writer.WriteLine($Json)
      $global:SunConn.Writer.Flush()
      Write-Log ("Sent line of {0} chars" -f $Json.Length)
    } else {
      # Fallback: Write UTF-8 bytes and newline
      $bytes = [System.Text.Encoding]::UTF8.GetBytes($Json)
      $global:SunConn.Stream.Write($bytes, 0, $bytes.Length)
      $nl = [byte[]](10)
      $global:SunConn.Stream.Write($nl, 0, 1)
      $global:SunConn.Stream.Flush()
      $len = $bytes.Length
      Write-Log ("Sent {0} bytes (+newline)" -f $len)
    }
  } catch {
    try { $global:SunConn.Stream.Dispose() } catch {}
    $global:SunConn = $null
    Write-Log "Send failed, resetting connection: $($_.Exception.Message)"
  }
}

function Get-PlayniteCategories {
  if (-not $PlayniteApi) { return @() }
  $cats = @()
  foreach ($c in $PlayniteApi.Database.Categories) {
    $cats += @{ id = $c.Id.ToString(); name = $c.Name }
  }
  Write-Log "Collected $($cats.Count) categories"
  return $cats
}

function Get-CategoryNamesMap {
  $map = @{}
  foreach ($c in $PlayniteApi.Database.Categories) { $map[$c.Id] = $c.Name }
  return $map
}

function Get-GameActionInfo {
  param([object]$Game)
  # Best-effort extraction of primary play action
  $exe = ''
  $args = ''
  $workDir = ''
  try {
    $actions = $Game.GameActions
    if ($actions -and $actions.Count -gt 0) {
      $play = $actions | Where-Object { $_.IsPlayAction } | Select-Object -First 1
      if (-not $play) { $play = $actions[0] }
      if ($play) {
        if ($play.Path) { $exe = $play.Path }
        if ($play.Arguments) { $args = $play.Arguments }
        if ($play.WorkingDir) { $workDir = $play.WorkingDir }
      }
    }
  } catch {}
  if (-not $workDir -and $Game.InstallDirectory) { $workDir = $Game.InstallDirectory }
  return @{ exe = $exe; args = $args; workingDir = $workDir }
}

function Get-BoxArtPath {
  param([object]$Game)
  try {
    if ($Game.CoverImage) {
      return $PlayniteApi.Database.GetFullFilePath($Game.CoverImage)
    }
  } catch {}
  return ''
}

function Get-PlayniteGames {
  if (-not $PlayniteApi) { return @() }
  $catMap = Get-CategoryNamesMap
  $games = @()
  foreach ($g in $PlayniteApi.Database.Games) {
    $act = Get-GameActionInfo -Game $g
    $catNames = @()
    if ($g.CategoryIds) { foreach ($cid in $g.CategoryIds) { if ($catMap.ContainsKey($cid)) { $catNames += $catMap[$cid] } } }
    $playtimeMin = 0
    try { if ($g.Playtime) { $playtimeMin = [int]([double]$g.Playtime / 60.0) } } catch {}
    $lastPlayed = ''
    try { if ($g.LastActivity) { $lastPlayed = ([DateTime]$g.LastActivity).ToString('o') } } catch {}
    $boxArt = Get-BoxArtPath -Game $g
    $games += @{
      id = $g.Id.ToString()
      name = $g.Name
      exe = $act.exe
      args = $act.args
      workingDir = $act.workingDir
      categories = $catNames
      playtimeMinutes = $playtimeMin
      lastPlayed = $lastPlayed
      boxArtPath = $boxArt
      description = $g.Description
      tags = @() # TODO: fill from $g.TagIds if needed
    }
  }
  Write-Log "Collected $($games.Count) games"
  return $games
}

function Send-InitialSnapshot {
  Write-Log "Building initial snapshot"
  $categories = Get-PlayniteCategories
  $json = @{ type = 'categories'; payload = $categories } | ConvertTo-Json -Depth 6 -Compress
  Send-JsonMessage -Json $json
  $catCount = $categories.Count
  Write-Log ("Sent categories snapshot ({0})" -f $catCount)

  $games = Get-PlayniteGames
  # Send in batches to avoid overly large messages
  $batchSize = 100
  for ($i = 0; $i -lt $games.Count; $i += $batchSize) {
    $chunk = $games[$i..([Math]::Min($i + $batchSize - 1, $games.Count - 1))]
    $jsonG = @{ type = 'games'; payload = $chunk } | ConvertTo-Json -Depth 8 -Compress
    Send-JsonMessage -Json $jsonG
    $batchIndex = [int]([double]$i / $batchSize) + 1
    $chunkCount = $chunk.Count
    Write-Log ("Sent games batch {0} with {1} items" -f $batchIndex, $chunkCount)
  }
  $gamesCount = $games.Count
  Write-Log ("Initial snapshot completed: categories={0} games={1}" -f $catCount, $gamesCount)
}

function Start-ConnectorLoop {
  while ($true) {
    try {
      Write-Log "Connector loop: attempting connection"
      $global:SunConn = Connect-SunshinePipe -TimeoutMs 10000
      Write-Log "Connector loop: connected"
      Send-InitialSnapshot
      # Reader loop: handle simple line-delimited command messages from Sunshine
      while ($global:SunConn -and $global:SunConn.Stream -and $global:SunConn.Stream.CanRead) {
        try {
          $line = $global:SunConn.Reader.ReadLine()
        } catch {
          Write-Log "Reader exception: $($_.Exception.Message)"
          break
        }
        if (-not $line) { Start-Sleep -Milliseconds 200; continue }
        $lineLen = $line.Length
        Write-Log ("Received line ({0} chars)" -f $lineLen)
        try {
          $obj = $line | ConvertFrom-Json -ErrorAction Stop
          if ($obj.type -eq 'command' -and $obj.command -eq 'launch' -and $obj.id) {
            try {
              $gid = [Guid]$obj.id
              $game = $null
              foreach ($g in $PlayniteApi.Database.Games) { if ($g.Id -eq $gid) { $game = $g; break } }
              if ($game) { Write-Log "Launching game $($game.Name) [$($game.Id)]"; $PlayniteApi.StartGame($game) } else { Write-Log "Launch command received but game not found: $($obj.id)" }
            } catch {}
          }
        } catch { Write-Log "Failed to parse/handle line: $($_.Exception.Message)" }
      }
    } catch {
      Write-Log "Connector loop: connection attempt failed: $($_.Exception.Message)"
    }
    Start-Sleep -Seconds 3
  }
}

# Synchronous single-shot connection probe to ensure logging works even before background loop
function Try-ConnectOnce {
  try {
    Write-Log "Try-ConnectOnce: attempting initial public pipe connect"
    $tmp = Connect-SunshinePipe -TimeoutMs 8000
    if ($tmp -and $tmp.Stream -and $tmp.Stream.CanWrite) {
      $global:SunConn = $tmp
      try {
        Send-InitialSnapshot
        Write-Log "Try-ConnectOnce: initial snapshot sent"
      } catch {
        Write-Log "Try-ConnectOnce: failed to send initial snapshot: $($_.Exception.Message)"
      }
    } else {
      Write-Log "Try-ConnectOnce: connection object invalid"
    }
  } catch {
    Write-Log "Try-ConnectOnce: failed: $($_.Exception.Message)"
  }
}

function OnApplicationStarted() {
  try {
    Initialize-Logging
    Write-Log "OnApplicationStarted invoked"
    # First, do a synchronous initial connect + snapshot for immediate availability
    Try-ConnectOnce
    # Then start background connector for ongoing read + self-healing reconnection
    $null = [System.Threading.Tasks.Task]::Run([Action]{ Start-ConnectorLoop })
    Write-Log "OnApplicationStarted: background connector started"
  } catch {
    # Fallback: run synchronously once
    Write-Log "OnApplicationStarted: background task failed, running synchronously"
    Start-ConnectorLoop
  }
}

function Send-StatusMessage {
  param([string]$Name, [object]$Game)
  $payload = @{ type = 'status'; status = @{ name = $Name; id = $Game.Id.ToString(); installDir = $Game.InstallDirectory; exe = (Get-GameActionInfo -Game $Game).exe } } | ConvertTo-Json -Depth 5 -Compress
  Send-JsonMessage -Json $payload
}

function OnGameStarted() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStarted: $($game.Name) [$($game.Id)]"
  Send-StatusMessage -Name 'gameStarted' -Game $game
}

function OnGameStopped() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStopped: $($game.Name) [$($game.Id)]"
  Send-StatusMessage -Name 'gameStopped' -Game $game
}

# Optional: clean up on exit
function OnApplicationStopped() {
  try {
    if ($global:SunConn -and $global:SunConn.Stream) { $global:SunConn.Stream.Dispose() }
  } catch {}
  $global:SunConn = $null
  Write-Log "OnApplicationStopped: connector stopped"
}
