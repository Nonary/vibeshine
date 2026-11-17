$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\packaging\windows\vibe-speakers\Setup\Common.ps1')
$facts=[pscustomobject]@{Windows11X64=$true;CodeIntegrityOptions=1;VbsStatus=0;HvciRunning=$false;SecureBoot=1;AdminBootChecked=$true;TestSigning=$false;NoIntegrityChecks=$false;DriverConflicts=@();UsbIdentityConflicts=@()}
Require ((CompatibilityErrors $facts).Count -eq 0) 'A supported CI-enforced machine must not require HVCI to be enabled'
$facts.HvciRunning=$true;$facts.VbsStatus=2
Require ((CompatibilityErrors $facts).Count -eq 0) 'HVCI configuration rejected'
foreach($pair in @(@('CodeIntegrityOptions',0),@('CodeIntegrityOptions',3),@('TestSigning',$true),@('NoIntegrityChecks',$true),@('SecureBoot',-1),@('DriverConflicts',@('wrong signed package')), @('UsbIdentityConflicts',@('foreign device')))){
 $old=$facts.($pair[0]);$facts.($pair[0])=$pair[1]
 Require ((CompatibilityErrors $facts).Count -gt 0) ('Invalid setup accepted: '+$pair[0])
 $facts.($pair[0])=$old
}
$errors=$null;$tokens=$null
foreach($file in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '..\..\packaging\windows\vibe-speakers') -Filter '*.ps1' -Recurse){
 $null=[Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)
 Require ($errors.Count -eq 0) ('PowerShell syntax: '+$file.Name+': '+($errors -join '; '))
}
Write-Output 'PASS actual setup compatibility policy and all product PowerShell syntax'
