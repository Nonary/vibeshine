[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageRoot)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($PackageRoot).TrimEnd('\')
$paths=@('sunshine.exe','tools\VibeUsbBridge.exe')
foreach($folder in 'VibeSpeakers','drivers\usbip-win2') {
 $paths+=@(Get-ChildItem -LiteralPath (Join-Path $root $folder) -File -Recurse |
  Where-Object Name -ne 'payload.lock.json' | ForEach-Object {$_.FullName.Substring($root.Length+1)})
}
$rows=@(foreach($relative in $paths|Sort-Object -Unique) {
 $file=Join-Path $root $relative
 $item=Get-Item -LiteralPath $file
 if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse payload refused'}
 [ordered]@{Path=$relative;Bytes=$item.Length;SHA256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
})
[ordered]@{Schema='vibe-speakers-rc1-payload/1';PrivateIdentity='1209:7140 pending; no public binary release';Files=$rows} |
 ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'VibeSpeakers\payload.lock.json') -Encoding UTF8
