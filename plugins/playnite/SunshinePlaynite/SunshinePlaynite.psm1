<#
  Sunshine Playnite Connector (PowerShell Script Extension)
  - Single-threaded design using UI DispatcherTimer (no jobs/runspaces)
  - Connect to public control pipe, perform anonymous handshake to secured data pipe
  - Send categories and games on an interval; handle launch commands inline
  - Fallback compatible with servers using a single public pipe (no handshake)

  Requires: Playnite script extension context (access to $PlayniteApi)
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$global:SunConn = $null
$script:InitialSnapshotSent = $false
$script:LastSnapshotAt = [DateTime]::MinValue
$script:SnapshotIntervalSeconds = 30
$script:IsGameRunning = $false

# Logger state
$script:LogPath = $null
$script:HostLogger = $null

# Connection state (single-threaded)
$script:ControlPipeName = 'sunshine_playnite_connector'
$script:PipeStream = $null
$script:PipeWriter = $null
$script:RecvBuffer = ''
$script:TickTimer = $null

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

# (Removed jobs/runspaces; everything runs on UI DispatcherTimer tick)

#
# Thread marshalling helpers
# Ensure all interactions with Playnite API occur on the UI/main thread.
#
function Invoke-PlayniteMain {
  param([ScriptBlock]$Script)
  # Try WPF Dispatcher first (Playnite is a WPF app), then Playnite API's InvokeOnMainThread, else run inline
  $script:__inv_inner = $Script
  try {
    # Attempt to use WPF Dispatcher if available
    try { $null = [System.Windows.Application] } catch { try { [void][System.Reflection.Assembly]::Load('PresentationFramework') } catch {} }
    if ([System.Windows.Application]::Current -and [System.Windows.Application]::Current.Dispatcher) {
      $disp = [System.Windows.Application]::Current.Dispatcher
      if ($disp.CheckAccess()) {
        return (& $script:__inv_inner)
      } else {
        $script:__inv_tmp = $null
        $disp.Invoke([System.Action]{ $script:__inv_tmp = (& $script:__inv_inner) })
        $res = $script:__inv_tmp
        Remove-Variable -Name __inv_tmp -Scope Script -ErrorAction SilentlyContinue
        return $res
      }
    }
  } catch {
    # Ignore and try Playnite API path or fallback
  }
  try {
    if ($PlayniteApi -and ($PlayniteApi.PSObject.Methods.Name -contains 'InvokeOnMainThread')) {
      $script:__inv_tmp2 = $null
      $PlayniteApi.InvokeOnMainThread([System.Action]{ $script:__inv_tmp2 = (& $script:__inv_inner) })
      $res2 = $script:__inv_tmp2
      Remove-Variable -Name __inv_tmp2 -Scope Script -ErrorAction SilentlyContinue
      return $res2
    }
  } catch {
    # Ignore and fallback to direct invocation
  } finally {
    Remove-Variable -Name __inv_inner -Scope Script -ErrorAction SilentlyContinue
  }
  # Fallback: run directly
  return (& $Script)
}

function Invoke-PlayniteMainAsync {
  param([ScriptBlock]$Script)
  try {
    try { $null = [System.Windows.Application] } catch { try { [void][System.Reflection.Assembly]::Load('PresentationFramework') } catch {} }
    if ([System.Windows.Application]::Current -and [System.Windows.Application]::Current.Dispatcher) {
      $disp = [System.Windows.Application]::Current.Dispatcher
      [void]$disp.BeginInvoke([System.Action]{ & $Script })
      return
    }
  } catch {}
  # Fallback to synchronous
  try { & $Script } catch {}
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

# Extra P/Invoke for nonblocking peeks
try {
  if (-not ([System.Management.Automation.PSTypeName]'Win32.NativePeek').Type) {
    Add-Type -Namespace Win32 -Name NativePeek -MemberDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativePeek {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool PeekNamedPipe(IntPtr hNamedPipe, IntPtr lpBuffer, uint nBufferSize, IntPtr lpBytesRead, out uint lpTotalBytesAvail, IntPtr lpBytesLeftThisMessage);
}
"@
    Write-Log "Loaded Win32.NativePeek for PeekNamedPipe"
  }
} catch {
  Write-Log "Failed to load Win32.NativePeek: $($_.Exception.Message)"
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

function Connect-ControlPipe {
  param([int]$TimeoutMs = 3000)
  $PublicName = $script:ControlPipeName
  try {
    Write-Log "Connecting control pipe '$PublicName' (timeout=${TimeoutMs}ms)"
    if (([System.Management.Automation.PSTypeName]'Win32.NativeMethods').Type) {
      $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
      while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-PipeAvailable -Name $PublicName -TimeoutMs 250) { break }
        Start-Sleep -Milliseconds 100
      }
    }
    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', $PublicName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
    $pipe.Connect([Math]::Max(1000, [int]($TimeoutMs)))
    try { $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Byte } catch {}
    try { $pipe.ReadTimeout = 50 } catch {}
    try { $pipe.WriteTimeout = 2000 } catch {}
    Write-Log "Control pipe connected: CanRead=$($pipe.CanRead) CanWrite=$($pipe.CanWrite)"
    return $pipe
  } catch {
    if ($pipe) { try { $pipe.Dispose() } catch {} }
    Write-Log "Control connect failed: $($_.Exception.Message)"; return $null
  }
}

function Try-Receive-Handshake {
  param([System.IO.Pipes.PipeStream]$ControlPipe, [int]$TimeoutMs = 1500)
  # Expect 80-byte struct of wchar[40] containing the data pipe name
  $targetSize = 80
  $t0 = [DateTime]::UtcNow
  $acc = New-Object byte[] $targetSize
  $off = 0
  while (([DateTime]::UtcNow - $t0).TotalMilliseconds -lt $TimeoutMs -and $off -lt $targetSize) {
    try {
      if (([System.Management.Automation.PSTypeName]'Win32.NativePeek').Type) {
        $h = $ControlPipe.SafePipeHandle.DangerousGetHandle()
        $avail = 0u
        [void][Win32.NativePeek]::PeekNamedPipe($h, [IntPtr]::Zero, 0u, [IntPtr]::Zero, [ref]$avail, [IntPtr]::Zero)
        if ($avail -lt 1) { Start-Sleep -Milliseconds 25; continue }
      }
      $read = $ControlPipe.Read($acc, $off, ($targetSize - $off))
      if ($read -le 0) { Start-Sleep -Milliseconds 25; continue }
      $off += $read
    } catch {
      Start-Sleep -Milliseconds 25
    }
  }
  if ($off -lt $targetSize) { return $null }
  # Decode UTF-16LE wide string up to first NUL
  try {
    $s = [System.Text.Encoding]::Unicode.GetString($acc, 0, $targetSize)
    $nul = $s.IndexOf([char]0)
    if ($nul -gt -1) { $s = $s.Substring(0, $nul) }
    $s = $s.Trim()
    if (-not [string]::IsNullOrWhiteSpace($s)) { return $s }
  } catch {}
  return $null
}

function Send-Handshake-Ack {
  param([System.IO.Pipes.PipeStream]$ControlPipe)
  try {
    $b = [byte[]](0x02)
    $ControlPipe.Write($b, 0, 1)
    $ControlPipe.Flush()
    Write-Log "Sent handshake ACK"
  } catch {
    Write-Log "Failed to send handshake ACK: $($_.Exception.Message)"
  }
}

function Ensure-DataConnection {
  # Returns $true if connected and writer ready
  if ($script:PipeStream -and $script:PipeStream.IsConnected) { return $true }
  # Clean up
  try { if ($script:PipeWriter) { $script:PipeWriter.Dispose(); $script:PipeWriter = $null } } catch {}
  try { if ($script:PipeStream) { $script:PipeStream.Dispose(); $script:PipeStream = $null } } catch {}

  $ctrl = Connect-ControlPipe -TimeoutMs 3000
  if (-not $ctrl) { return $false }

  $dataName = Try-Receive-Handshake -ControlPipe $ctrl -TimeoutMs 1500
  if ($dataName) {
    Write-Log "Handshake received; data pipe name='$dataName'"
    Send-Handshake-Ack -ControlPipe $ctrl
    try { $ctrl.Dispose() } catch {}
    try {
      $dp = [System.IO.Pipes.NamedPipeClientStream]::new('.', $dataName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
      $dp.Connect(5000)
      try { $dp.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Byte } catch {}
      try { $dp.ReadTimeout = 10 } catch {}
      try { $dp.WriteTimeout = 2000 } catch {}
      $script:PipeStream = $dp
      $script:PipeWriter = New-Object System.IO.StreamWriter($dp, [System.Text.Encoding]::UTF8, 8192, $true)
      $script:PipeWriter.AutoFlush = $true
      $script:RecvBuffer = ''
      Write-Log "Connected to secured data pipe"
      return $true
    } catch {
      Write-Log "Failed to connect data pipe: $($_.Exception.Message)"
      try { $ctrl.Dispose() } catch {}
      return $false
    }
  } else {
    # Fallback: use control pipe for data (single-pipe servers)
    Write-Log "No handshake received; using control pipe as data pipe"
    $script:PipeStream = $ctrl
    $script:PipeWriter = New-Object System.IO.StreamWriter($ctrl, [System.Text.Encoding]::UTF8, 8192, $true)
    $script:PipeWriter.AutoFlush = $true
    $script:RecvBuffer = ''
    return $true
  }
}

function Send-JsonLine {
  param([Parameter(Mandatory)][string]$Json)
  if (-not (Ensure-DataConnection)) { Write-Log "Send: not connected"; return }
  try {
    $script:PipeWriter.WriteLine($Json)
    $script:PipeWriter.Flush()
    Write-Log ("Sent line of {0} chars" -f $Json.Length)
  } catch {
    Write-Log "Send failed: $($_.Exception.Message)"
    try { if ($script:PipeStream) { $script:PipeStream.Dispose() } } catch {}
    $script:PipeStream = $null; $script:PipeWriter = $null
  }
}

function Get-PlayniteCategories {
  if (-not $PlayniteApi) { return @() }
  $result = Invoke-PlayniteMain {
    $cats = @()
    foreach ($c in $PlayniteApi.Database.Categories) {
      $cats += @{ id = $c.Id.ToString(); name = $c.Name }
    }
    $cats
  }
  try { Write-Log "Collected $($result.Count) categories" } catch {}
  return $result
}

function Get-CategoryNamesMap {
  if (-not $PlayniteApi) { return @{} }
  return (Invoke-PlayniteMain {
    $map = @{}
    foreach ($c in $PlayniteApi.Database.Categories) { $map[$c.Id] = $c.Name }
    $map
  })
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
      $script:__img = $Game.CoverImage
      $val = Invoke-PlayniteMain { $PlayniteApi.Database.GetFullFilePath($script:__img) }
      Remove-Variable -Name __img -Scope Script -ErrorAction SilentlyContinue
      return $val
    }
  } catch {}
  return ''
}

function Get-PlayniteGames {
  if (-not $PlayniteApi) { return @() }
  $catMap = Get-CategoryNamesMap
  $script:__catMap = $catMap
  $result = Invoke-PlayniteMain {
    $games = @()
    foreach ($g in $PlayniteApi.Database.Games) {
      $act = Get-GameActionInfo -Game $g
      $catNames = @()
      if ($g.CategoryIds) { foreach ($cid in $g.CategoryIds) { if ($script:__catMap.ContainsKey($cid)) { $catNames += $script:__catMap[$cid] } } }
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
    $games
  }
  Remove-Variable -Name __catMap -Scope Script -ErrorAction SilentlyContinue
  try { Write-Log "Collected $($result.Count) games" } catch {}
  return $result
}

function Send-InitialSnapshot {
  Write-Log "Building initial snapshot"
  $categories = Get-PlayniteCategories
  $json = @{ type = 'categories'; payload = $categories } | ConvertTo-Json -Depth 6 -Compress
  Send-JsonLine -Json $json
  $catCount = $categories.Count
  Write-Log ("Sent categories snapshot ({0})" -f $catCount)

  $games = Get-PlayniteGames
  # Send in batches to avoid overly large messages
  $batchSize = 100
  for ($i = 0; $i -lt $games.Count; $i += $batchSize) {
    $chunk = $games[$i..([Math]::Min($i + $batchSize - 1, $games.Count - 1))]
    $jsonG = @{ type = 'games'; payload = $chunk } | ConvertTo-Json -Depth 8 -Compress
    Send-JsonLine -Json $jsonG
    $batchIndex = [int]([double]$i / $batchSize) + 1
    $chunkCount = $chunk.Count
    Write-Log ("Sent games batch {0} with {1} items" -f $batchIndex, $chunkCount)
  }
  $gamesCount = $games.Count
  Write-Log ("Initial snapshot completed: categories={0} games={1}" -f $catCount, $gamesCount)
  $script:InitialSnapshotSent = $true
  $script:LastSnapshotAt = [DateTime]::UtcNow
}

function Maybe-SendSnapshot {
  param([switch]$Force)
  if ($Force) { Send-InitialSnapshot; return }
  if (-not $script:InitialSnapshotSent) { Send-InitialSnapshot; return }
  if ($script:IsGameRunning) { return }
  try {
    $now = [DateTime]::UtcNow
    $elapsed = ($now - $script:LastSnapshotAt).TotalSeconds
    if ($elapsed -ge $script:SnapshotIntervalSeconds) { Send-InitialSnapshot }
  } catch { Send-InitialSnapshot }
}

function Drain-IncomingMessages {
  if (-not $script:PipeStream -or -not $script:PipeStream.IsConnected) { return }
  try {
    $avail = 0u
    if (([System.Management.Automation.PSTypeName]'Win32.NativePeek').Type) {
      $h = $script:PipeStream.SafePipeHandle.DangerousGetHandle()
      [void][Win32.NativePeek]::PeekNamedPipe($h, [IntPtr]::Zero, 0u, [IntPtr]::Zero, [ref]$avail, [IntPtr]::Zero)
      if ($avail -eq 0u) { return }
    }
    $toRead = [Math]::Min([int]$avail, 4096)
    if ($toRead -le 0) { $toRead = 4096 }
    $buf = New-Object byte[] $toRead
    $n = $script:PipeStream.Read($buf, 0, $buf.Length)
    if ($n -le 0) { return }
    $chunk = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
    $script:RecvBuffer += $chunk
    while ($true) {
      $idx = $script:RecvBuffer.IndexOf("`n")
      if ($idx -lt 0) { break }
      $line = $script:RecvBuffer.Substring(0, $idx)
      if ($line.EndsWith("`r")) { $line = $line.Substring(0, $line.Length-1) }
      $script:RecvBuffer = $script:RecvBuffer.Substring($idx + 1)
      if (-not [string]::IsNullOrWhiteSpace($line)) {
        try { $obj = $line | ConvertFrom-Json -ErrorAction Stop } catch { continue }
        if ($obj.type -eq 'command' -and $obj.command -eq 'launch' -and $obj.id) {
          try { $gid = [Guid]$obj.id } catch { $gid = $null }
          if ($gid) {
            try { $PlayniteApi.StartGame($gid) } catch {}
          }
        }
      }
    }
  } catch {
    Write-Log "Drain error: $($_.Exception.Message)"
    try { $script:PipeStream.Dispose() } catch {}
    $script:PipeStream = $null; $script:PipeWriter = $null
  }
}

function OnApplicationStarted() {
  try {
    Initialize-Logging
    Write-Log "OnApplicationStarted invoked"
    # Setup a single-threaded UI dispatcher timer
    try { $null = [System.Windows.Application] } catch { try { [void][System.Reflection.Assembly]::Load('PresentationFramework') } catch {} }
    $script:TickTimer = New-Object System.Windows.Threading.DispatcherTimer
    $script:TickTimer.Interval = [TimeSpan]::FromMilliseconds(250)
    $script:TickTimer.Add_Tick({
      if (-not (Ensure-DataConnection)) { return }
      Drain-IncomingMessages
      Maybe-SendSnapshot
    })
    $script:TickTimer.Start()
    Write-Log "OnApplicationStarted: dispatcher timer started"
  } catch {
    Write-Log "OnApplicationStarted: failed to start dispatcher: $($_.Exception.Message)"
  }
}

function Send-StatusMessage {
  param([string]$Name, [object]$Game)
  $payload = @{ type = 'status'; status = @{ name = $Name; id = $Game.Id.ToString(); installDir = $Game.InstallDirectory; exe = (Get-GameActionInfo -Game $Game).exe } } | ConvertTo-Json -Depth 5 -Compress
  Send-JsonLine -Json $payload
}

function OnGameStarted() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStarted: $($game.Name) [$($game.Id)]"
  $script:IsGameRunning = $true
  Send-StatusMessage -Name 'gameStarted' -Game $game
}

function OnGameStopped() {
  param($evnArgs)
  $game = $evnArgs.Game
  Write-Log "OnGameStopped: $($game.Name) [$($game.Id)]"
  $script:IsGameRunning = $false
  Send-StatusMessage -Name 'gameStopped' -Game $game
}

# Optional: clean up on exit
function OnApplicationStopped() {
  try {
    if ($script:TickTimer) { try { $script:TickTimer.Stop() } catch {}; $script:TickTimer = $null }
    if ($global:SunConn -and $global:SunConn.Stream) { $global:SunConn.Stream.Dispose() }
  } catch {}
  $global:SunConn = $null
  try { if ($script:PipeWriter) { $script:PipeWriter.Dispose() } } catch {}
  try { if ($script:PipeStream) { $script:PipeStream.Dispose() } } catch {}
  $script:PipeStream = $null; $script:PipeWriter = $null; $script:RecvBuffer = ''
  Write-Log "OnApplicationStopped: connector stopped"
}
