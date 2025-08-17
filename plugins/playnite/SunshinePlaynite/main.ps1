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

function Connect-SunshinePipe {
  param(
    [string]$ControlName = 'sunshine_playnite_connector',
    [int]$TimeoutMs = 5000
  )

  try {
    $ctrl = New-Object System.IO.Pipes.NamedPipeClientStream('.', $ControlName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
    $ctrl.Connect($TimeoutMs)

    $br = New-Object System.IO.BinaryReader($ctrl, [System.Text.Encoding]::Unicode)
    $bw = New-Object System.IO.BinaryWriter($ctrl)

    # Expect 40 WCHARs (80 bytes) for pipe name
    $raw = $br.ReadBytes(80)
    if ($raw.Length -lt 80) { throw "Handshake message too short ($($raw.Length) bytes)." }
    $pipeName = [System.Text.Encoding]::Unicode.GetString($raw).Trim([char]0)
    if ([string]::IsNullOrWhiteSpace($pipeName)) { throw "Invalid data pipe name received." }

    # Send ACK (0x02)
    $bw.Write([byte]0x02)
    $bw.Flush()
  } catch {
    if ($ctrl) { $ctrl.Dispose() }
    throw
  }

  try { $ctrl.Dispose() } catch {}

  # Connect data pipe
  $data = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
  $data.Connect($TimeoutMs)
  $writer = New-Object System.IO.StreamWriter($data, [System.Text.Encoding]::UTF8, 8192, $true)
  $writer.AutoFlush = $true
  $reader = New-Object System.IO.StreamReader($data, [System.Text.Encoding]::UTF8, $false, 8192, $true)

  return @{ Stream = $data; Writer = $writer; Reader = $reader }
}

function Send-JsonMessage {
  param(
    [Parameter(Mandatory)] [string]$Json
  )
  if (-not $global:SunConn -or -not $global:SunConn.Stream -or -not $global:SunConn.Stream.CanWrite) {
    try { $global:SunConn = Connect-SunshinePipe -TimeoutMs 3000 } catch { return }
  }
  try {
    # Write UTF-8 bytes without BOM
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Json)
    $global:SunConn.Stream.Write($bytes, 0, $bytes.Length)
    $global:SunConn.Stream.Flush()
  } catch {
    try { $global:SunConn.Stream.Dispose() } catch {}
    $global:SunConn = $null
  }
}

function Get-PlayniteCategories {
  if (-not $PlayniteApi) { return @() }
  $cats = @()
  foreach ($c in $PlayniteApi.Database.Categories) {
    $cats += @{ id = $c.Id.ToString(); name = $c.Name }
  }
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
  return $games
}

function Send-InitialSnapshot {
  $categories = Get-PlayniteCategories
  $json = @{ type = 'categories'; payload = $categories } | ConvertTo-Json -Depth 6
  Send-JsonMessage -Json $json

  $games = Get-PlayniteGames
  # Send in batches to avoid overly large messages
  $batchSize = 100
  for ($i = 0; $i -lt $games.Count; $i += $batchSize) {
    $chunk = $games[$i..([Math]::Min($i + $batchSize - 1, $games.Count - 1))]
    $jsonG = @{ type = 'games'; payload = $chunk } | ConvertTo-Json -Depth 8
    Send-JsonMessage -Json $jsonG
  }
}

function Start-ConnectorLoop {
  while ($true) {
    try {
      $global:SunConn = Connect-SunshinePipe -TimeoutMs 5000
      Send-InitialSnapshot
      # Reader loop: handle simple line-delimited command messages from Sunshine
      while ($global:SunConn -and $global:SunConn.Stream -and $global:SunConn.Stream.CanRead) {
        try {
          $line = $global:SunConn.Reader.ReadLine()
        } catch {
          break
        }
        if (-not $line) { Start-Sleep -Milliseconds 200; continue }
        try {
          $obj = $line | ConvertFrom-Json -ErrorAction Stop
          if ($obj.type -eq 'command' -and $obj.command -eq 'launch' -and $obj.id) {
            try {
              $gid = [Guid]$obj.id
              $game = $null
              foreach ($g in $PlayniteApi.Database.Games) { if ($g.Id -eq $gid) { $game = $g; break } }
              if ($game) { $PlayniteApi.StartGame($game) }
            } catch {}
          }
        } catch {}
      }
    } catch {
      # ignore and retry
    }
    Start-Sleep -Seconds 3
  }
}

function OnApplicationStarted() {
  try {
    # Run connector loop on a background task
    $null = [System.Threading.Tasks.Task]::Run([Action]{ Start-ConnectorLoop })
  } catch {
    # Fallback: run synchronously once
    Start-ConnectorLoop
  }
}

function Send-StatusMessage {
  param([string]$Name, [object]$Game)
  $payload = @{ type = 'status'; status = @{ name = $Name; id = $Game.Id.ToString(); installDir = $Game.InstallDirectory; exe = (Get-GameActionInfo -Game $Game).exe } } | ConvertTo-Json -Depth 5
  Send-JsonMessage -Json $payload
}

function OnGameStarted($game) {
  Send-StatusMessage -Name 'gameStarted' -Game $game
}

function OnGameStopped($game, $args) {
  Send-StatusMessage -Name 'gameStopped' -Game $game
}
