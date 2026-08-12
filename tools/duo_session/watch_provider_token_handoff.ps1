param(
    [Parameter(Mandatory = $true)]
    [string] $ProviderLog,

    [Parameter(Mandatory = $true)]
    [int] $SourceProcessId,

    [Parameter(Mandatory = $true)]
    [int] $SourceSessionId,

    [Parameter(Mandatory = $true)]
    [int] $TargetSessionId,

    [Parameter(Mandatory = $true)]
    [string] $ProofExe,

    [Parameter(Mandatory = $true)]
    [string] $ProofResult,

    [Parameter(Mandatory = $true)]
    [string] $LaunchResult,

    [Parameter(Mandatory = $true)]
    [string] $WatcherResult
)

$ErrorActionPreference = 'Stop'
$startedAt = Get-Date
$startLength = (Get-Item -LiteralPath $ProviderLog).Length
$deadline = $startedAt.AddSeconds(45)
$matchedLine = $null

while ((Get-Date) -lt $deadline -and -not $matchedLine) {
    $stream = [System.IO.File]::Open(
        $ProviderLog,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    try {
        $null = $stream.Seek($startLength, [System.IO.SeekOrigin]::Begin)
        $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8, $true, 4096, $true)
        try {
            $appended = $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
        $startLength = $stream.Position
    }
    finally {
        $stream.Dispose()
    }

    foreach ($line in ($appended -split "`r?`n")) {
        if ($line -match "ProtocolConnection::NotifySessionId session=$TargetSessionId(?:\s|$)") {
            $matchedLine = $line
            break
        }
    }

    if (-not $matchedLine) {
        Start-Sleep -Milliseconds 10
    }
}

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("Watcher.Identity=$([System.Security.Principal.WindowsIdentity]::GetCurrent().Name)")
$lines.Add("Watcher.SessionId=$([System.Diagnostics.Process]::GetCurrentProcess().SessionId)")
$lines.Add("Watcher.Started=$($startedAt.ToString('o'))")
$lines.Add("Watcher.Matched=$([bool]$matchedLine)")
$lines.Add("Watcher.MatchTime=$((Get-Date).ToString('o'))")
$lines.Add("Watcher.Line=$matchedLine")

if ($matchedLine) {
    & $ProofExe `
        --source-pid $SourceProcessId `
        --source-session $SourceSessionId `
        --target-session $TargetSessionId `
        --result $ProofResult `
        --launch-result $LaunchResult
    $lines.Add("Watcher.ProofExitCode=$LASTEXITCODE")
}

[System.IO.File]::WriteAllLines(
    $WatcherResult,
    $lines.ToArray(),
    [System.Text.UTF8Encoding]::new($false))
