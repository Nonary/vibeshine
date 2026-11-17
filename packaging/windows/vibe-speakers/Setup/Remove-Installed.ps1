# Called by the MSI after the service stops, before installed files are removed.
[CmdletBinding()]
param([string]$BootReceipt)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require (IsAdmin) 'Installer elevation required'
$users=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'VibeSpeakers\users'
if(!(Test-Path -LiteralPath $users)){return}
NoLinks $users
foreach($dir in Get-ChildItem -LiteralPath $users -Directory){
 NoLinks $dir.FullName
 $state=ReadState $dir.Name
 if(!$state -or $state.Phase -eq 'Removed'){continue}
 # The uninstall of another portable/install location cannot take this record.
 if(!$state.InstallRoot -or $state.InstallRoot -ine (PackageRoot)){continue}
 & (Join-Path $PSScriptRoot 'Invoke-AdminSetup.ps1') -Action Remove -TargetUserSid $dir.Name -BootReceipt $BootReceipt -ConfirmSetup
}
