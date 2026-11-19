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
$owned.Phase='Installing'
Require (OwnsInstallerTransaction $owned $owned.InstallRoot $transaction) 'Interrupted driver staging must remain eligible for rollback'
$owned.Phase='Ready'
foreach($case in @(@('CreatedByVibeshine',$false),@('InstallRoot','C:\Foreign'),@('Phase','Removed'),@('InstallerCommitted',$true),@('InstallerCommitted','false'),@('InstallerTransaction','{11234567-0123-4567-89AB-0123456789AB}'))){
 $old=$owned.($case[0]);$owned.($case[0])=$case[1]
 Require (!(OwnsInstallerTransaction $owned 'C:\Program Files\Sunshine' $transaction)) ('Unsafe installer rollback accepted: '+$case[0])
 $owned.($case[0])=$old
}
Require (!(OwnsInstallerTransaction ([pscustomobject]@{Phase='Ready'}) $owned.InstallRoot $transaction)) 'Pre-existing journal adopted by rollback'
Write-Output 'PASS actual MSI transaction ownership/commit/rollback decisions'
$now=[datetime]::UtcNow;$boot=$now.AddHours(-1).ToString('o')
$observation=[pscustomobject]@{Schema='vibe-speakers-installer-security/1';BootTime=$boot;ObservedUtc=$now.AddSeconds(-2).ToString('o');SecureBoot=$true;TestSigning=$false;NoIntegrityChecks=$false}
ValidateBootObservation $observation $boot 1 $now
foreach($case in @(@('Schema','other'),@('BootTime',$now.AddHours(-2).ToString('o')),@('ObservedUtc',$now.AddMinutes(-21).ToString('o')),@('ObservedUtc',$now.AddMinutes(1).ToString('o')),@('SecureBoot',$false),@('SecureBoot','true'),@('TestSigning',$true),@('TestSigning','false'),@('NoIntegrityChecks',$true))){
 $old=$observation.($case[0]);$observation.($case[0])=$case[1];$rejected=$false
 try{ValidateBootObservation $observation $boot 1 $now}catch{$rejected=$true}
 Require $rejected ('Invalid installer firmware observation accepted: '+$case[0]);$observation.($case[0])=$old
}
Write-Output 'PASS actual boot/age/firmware/type binding of installer security observations'

# Exercise the MSI entry point in Windows PowerShell without any device operations.
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('vibe-remove-test-'+[guid]::NewGuid().ToString('N'))
$user=Join-Path $testRoot 'users\S-1-5-21-111-222-333-1001'
$common=Join-Path $testRoot 'Common.ps1'
$helper=Join-Path $testRoot 'Invoke-AdminSetup.ps1'
$remover=Join-Path $testRoot 'Remove-Installed.ps1'
$log=Join-Path $testRoot 'removal.log'
try{
 $null=New-Item -ItemType Directory -Path $user -Force
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\..\packaging\windows\vibe-speakers\Setup\Remove-Installed.ps1') -Destination $remover
 @'
$ErrorActionPreference='Stop'
function IsAdmin { return $true }
function Require([bool]$condition,[string]$message){if(!$condition){throw $message}}
function NoLinks([string]$path){}
function PackageRoot { return $PSScriptRoot }
function ReadState([string]$sid){[pscustomobject]@{Phase='Ready';InstallRoot=$PSScriptRoot}}
function Join-Path([string]$Path,[string]$ChildPath){
 if($ChildPath -eq 'VibeSpeakers\users'){return "$PSScriptRoot\users"}
 Microsoft.PowerShell.Management\Join-Path $Path $ChildPath
}
'@ | Set-Content -LiteralPath $common -Encoding ascii
 $windowsPowerShell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
 foreach($expected in 0,1){
  $body=if($expected){'Write-Error "Controlled removal failure" -ErrorAction Continue; exit 1'}else{'return'}
  'param($Action,$TargetUserSid,[switch]$ConfirmSetup)',$body | Set-Content -LiteralPath $helper -Encoding ascii
  $oldPreference=$ErrorActionPreference
  try{
   $ErrorActionPreference='Continue'
   & $windowsPowerShell -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File $remover *> $log
   $code=$LASTEXITCODE
  }finally{$ErrorActionPreference=$oldPreference}
  Require ($code -eq $expected) "Removal entry point returned $code, expected $expected"
  if($expected){
   Require (Select-String -LiteralPath $log -Pattern 'Vibe Speakers removal failed' -Quiet) 'Removal error was not reported'
  }
 }
}finally{
 foreach($path in $log,$remover,$helper,$common,$user,(Split-Path $user),$testRoot){
  if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}
 }
}
Write-Output 'PASS MSI removal script propagates helper failure and accepts normal return'
