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
$transaction='{01234567-0123-4567-89AB-0123456789AB}'
$owned=[pscustomobject]@{CreatedByVibeshine=$true;InstallRoot='C:\Program Files\Sunshine';Phase='Ready';InstallerCommitted=$false;InstallerTransaction=$transaction}
Require (OwnsInstallerTransaction $owned $owned.InstallRoot $transaction) 'Fresh MSI transaction not owned'
foreach($case in @(@('CreatedByVibeshine',$false),@('InstallRoot','C:\Foreign'),@('Phase','Installing'),@('InstallerCommitted',$true),@('InstallerCommitted','false'),@('InstallerTransaction','{11234567-0123-4567-89AB-0123456789AB}'))){
 $old=$owned.($case[0]);$owned.($case[0])=$case[1]
 Require (!(OwnsInstallerTransaction $owned 'C:\Program Files\Sunshine' $transaction)) ('Unsafe installer rollback accepted: '+$case[0])
 $owned.($case[0])=$old
}
Require (!(OwnsInstallerTransaction ([pscustomobject]@{Phase='Ready'}) $owned.InstallRoot $transaction)) 'Pre-existing journal adopted by rollback'
Write-Output 'PASS actual MSI transaction ownership/commit/rollback decisions'
$now=[datetime]::UtcNow;$boot=$now.AddHours(-1).ToString('o')
$observation=[pscustomobject]@{Schema='vibe-speakers-installer-security/1';BootTime=$boot;ObservedUtc=$now.AddSeconds(-2).ToString('o');SecureBoot=$true}
ValidateBootObservation $observation $boot 1 $now
foreach($case in @(@('Schema','other'),@('BootTime',$now.AddHours(-2).ToString('o')),@('ObservedUtc',$now.AddMinutes(-21).ToString('o')),@('ObservedUtc',$now.AddMinutes(1).ToString('o')),@('SecureBoot',$false),@('SecureBoot','true'))){
 $old=$observation.($case[0]);$observation.($case[0])=$case[1];$rejected=$false
 try{ValidateBootObservation $observation $boot 1 $now}catch{$rejected=$true}
 Require $rejected ('Invalid installer firmware observation accepted: '+$case[0]);$observation.($case[0])=$old
}
Write-Output 'PASS actual boot/age/firmware/type binding of installer security observations'
