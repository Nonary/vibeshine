[CmdletBinding()]
param([ValidateSet('Install','Remove')][string]$Action='Install')
. (Join-Path $PSScriptRoot 'Common.ps1')
Write-Output 'Vibe Speakers RC1: private tests only. Requested 1209:7140 allocation is pending; no public redistribution.'
Write-Output $(if($Action -eq 'Install'){'Setup creates one owned USB/IP controller for this user. Signed shared packages are reused; incompatible versions are refused.'}else{'Removal affects only this installation-owned controller. Shared driver packages remain installed.'})
Write-Output 'No boot, Secure Boot, HVCI or signature-enforcement setting is changed.'
$answer=Read-Host 'Type YES to request administrator confirmation'
if($answer -cne 'YES'){Write-Output 'Cancelled. No setup action.';exit 0}
$sid=CurrentSid
$script=Join-Path $PSScriptRoot 'Invoke-AdminSetup.ps1'
$args='-NoProfile -ExecutionPolicy RemoteSigned -File "'+$script+'" -Action '+$Action+' -TargetUserSid '+$sid+' -ConfirmPrivateHardwareTest'
$p=Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList $args -Verb RunAs -WindowStyle Normal -PassThru
$p.WaitForExit();if($p.ExitCode -ne 0){throw ('Setup failed, exit '+$p.ExitCode+'. Preserve the setup journal and preflight output.')}
