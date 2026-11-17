[CmdletBinding()]
param([ValidateSet('Install','Remove','Preflight')][string]$Action='Preflight',
      [Parameter(Mandatory)][string]$TargetUserSid,[switch]$ConfirmPrivateHardwareTest)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require (IsAdmin) 'Administrator confirmation is required for this setup step'
if($Action -ne 'Preflight'){Require $ConfirmPrivateHardwareTest 'Private in-house test confirmation is missing'}
$targetAccount=([Security.Principal.SecurityIdentifier]::new($TargetUserSid)).Translate([Security.Principal.NTAccount]).Value
$stateDir=StateDirectory $TargetUserSid
function ProtectedAcl([bool]$directory){
 $acl=if($directory){[Security.AccessControl.DirectorySecurity]::new()}else{[Security.AccessControl.FileSecurity]::new()}
 $acl.SetAccessRuleProtection($true,$false);$acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
 foreach($sid in 'S-1-5-18','S-1-5-32-544',$TargetUserSid){
  $rights=if($sid -eq $TargetUserSid){'ReadAndExecute'}else{'FullControl'}
  $inherit=if($directory){'ContainerInherit,ObjectInherit'}else{'None'}
  $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid),$rights,$inherit,'None','Allow'))
 }
 return $acl
}
function CheckJournalDirectory([string]$path){
 NoLinks $path;$acl=Get-Acl -LiteralPath $path
 Require ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -in @('S-1-5-18','S-1-5-32-544')) 'Setup journal directory is not administrator-owned; do not take over foreign state'
 foreach($ace in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
  if($ace.AccessControlType -eq 'Allow' -and $ace.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544')){
   Require (($ace.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Write) -eq 0 -and ($ace.FileSystemRights -band [Security.AccessControl.FileSystemRights]::ChangePermissions) -eq 0 -and ($ace.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Delete) -eq 0 -and ($ace.FileSystemRights -band [Security.AccessControl.FileSystemRights]::TakeOwnership) -eq 0) 'Untrusted write access on setup journal'
  }
 }
}
function SaveProtected([string]$name,$value){
 CheckJournalDirectory $stateDir
 $file=Join-Path $stateDir $name
 if(Test-Path -LiteralPath $file){NoLinks $file}
 $temp=Join-Path $stateDir ([guid]::NewGuid().ToString('N')+'.tmp')
 $data=[Text.UTF8Encoding]::new($false).GetBytes(($value|ConvertTo-Json -Depth 15))
 $stream=[IO.FileStream]::new($temp,[IO.FileMode]::CreateNew,[Security.AccessControl.FileSystemRights]::WriteData,[IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough,(ProtectedAcl $false))
 try{$stream.Write($data,0,$data.Length);$stream.Flush($true)}finally{$stream.Dispose()}
 if(Test-Path -LiteralPath $file){[IO.File]::Replace($temp,$file,($file+'.previous-'+[guid]::NewGuid().ToString('N')+'.json'))}else{[IO.File]::Move($temp,$file)}
}
$facts=GetPreflight $TargetUserSid
Require $facts.Compatible ($facts.Errors -join '; ')
if($Action -eq 'Preflight'){$facts|ConvertTo-Json -Depth 12;return}
# Only our state namespace is created. Existing shared driver/controller state is never adopted.
foreach($path in @((Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'VibeSpeakers'),(Split-Path $stateDir),$stateDir)){
 if(!(Test-Path -LiteralPath $path)){$null=[IO.Directory]::CreateDirectory($path,(ProtectedAcl $true))}
 CheckJournalDirectory $path
}
$mutex=[Threading.Mutex]::new($false,'Global\VibeshineUsbIpInstall');$held=$false
try{
 try{$held=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{throw 'Previous setup was interrupted; inspect its journal before retrying'}
 Require $held 'Another USB/IP setup is active; no action performed'
 $state=ReadState $TargetUserSid
 $operation=[guid]::NewGuid().ToString('N')
 SaveProtected ($operation+'-preflight.json') $facts
 if($Action -eq 'Remove'){
  if(!$state -or $state.Phase -eq 'Removed'){Write-Output 'No owned RC1 controller to remove. Shared drivers remain installed.';return}
  Require (@(Get-Process sunshine -ErrorAction SilentlyContinue|Where-Object Path -IEQ (Join-Path (PackageRoot) 'sunshine.exe')).Count -eq 0) 'Stop this portable host with Stop.cmd before removing its controller'
  Require ($state.Phase -eq 'Ready') 'Interrupted setup state; inspect journal before removal'
  [VibeSpeakers.RC1.UsbControllerSetup]::Remove($state.ControllerInstance,$state.Token,$state.TargetSddl)
  $state.Phase='Removed';$state.RemovedUtc=[datetime]::UtcNow.ToString('o')
  SaveProtected 'setup.json' $state
  SaveProtected ($operation+'-removed.json') $state
  Write-Output 'Removed only this RC1 controller. No driver package, foreign device, boot option or audio role was changed.'
  return
 }
 if($state -and $state.Phase -eq 'Ready'){
  $null=[VibeSpeakers.RC1.UsbControllerSetup]::CheckMarker($state.ControllerInstance,$state.Token)
  Write-Output 'Reusing the exact journal-owned RC1 controller; no driver or controller change.'
 }else{
  Require (!$state -or $state.Phase -eq 'Removed') 'Incomplete setup journal; inspect it instead of creating another controller'
  Require (@(Get-Process sunshine -ErrorAction SilentlyContinue|Where-Object Path -IEQ (Join-Path (PackageRoot) 'sunshine.exe')).Count -eq 0) 'Stop this portable host before creating a new controller'
  $token=[guid]::NewGuid().ToString('N');$sddl=[VibeSpeakers.RC1.UsbIpDeviceSecurity]::Sddl($TargetUserSid)
  $state=[pscustomobject]@{Schema='vibe-speakers-rc1-setup/1';UserSid=$TargetUserSid;UserAccount=$targetAccount;Token=$token;TargetSddl=$sddl;
   Phase='Installing';ControllerInstance='';CreatedUtc=[datetime]::UtcNow.ToString('o');RemovedUtc=$null;CreatedByRc1=$true;SharedDriversRetained=$true;
   SetupBoot=$facts.BootTime;RebootRequired=$false}
  SaveProtected 'setup.json' $state
  $dep=Join-Path (PackageRoot) 'drivers\usbip-win2'
  foreach($name in 'usbip2_filter.inf','usbip2_ude.inf'){
   if(@($facts.StagedInf|Where-Object Original -eq $name).Count -eq 0){
    & "$env:SystemRoot\System32\pnputil.exe" /add-driver (Join-Path $dep $name)
    Require ($LASTEXITCODE -in @(0,3010)) ('Signed Driver Store staging failed: '+$name+'; no /install or shared replacement attempted')
    if($LASTEXITCODE -eq 3010){$state.RebootRequired=$true}
   }
  }
  try{
   $record=[Action[string]]{param($id) $state.ControllerInstance=$id;SaveProtected 'setup.json' $state}
   $created=[VibeSpeakers.RC1.UsbControllerSetup]::Create($token,$sddl,$record)
   $state.ControllerInstance=$created.InstanceId;$state.RebootRequired=$state.RebootRequired -or $created.RebootRequired
   SaveProtected 'setup.json' $state
   $snap=[VibeSpeakers.RC1.UsbIpDeviceSecurity]::Inspect()
   Require ([VibeSpeakers.RC1.UsbIpDeviceSecurity]::Equivalent($snap.Security,$sddl)) 'New root ACL does not match the approved user'
   Require ($snap.Started -and $snap.ProblemCode -eq 0) 'New root did not start; retain journal and inspect Code Integrity logs'
   $state.Phase='Ready';SaveProtected 'setup.json' $state
   SaveProtected ($operation+'-created.json') $state
  }catch{
   if([VibeSpeakers.RC1.UsbControllerSetup]::LastCreateRolledBack){$state.Phase='Removed';$state.RemovedUtc=[datetime]::UtcNow.ToString('o');SaveProtected 'setup.json' $state}
   SaveProtected ($operation+'-failure.json') @{Error=$_.Exception.ToString();State=$state;NoBootChange=$true;SharedDriversRetained=$true}
   throw
  }
 }
 $null=[VibeSpeakers.RC1.UsbControllerSetup]::CheckMarker($state.ControllerInstance,$state.Token)
 $bundle=[ordered]@{schema='vibe-speakers-runtime/1';bridge_sha256=(Hash (Join-Path (PackageRoot) 'tools\VibeUsbBridge.exe'));controller_instance=$state.ControllerInstance}
 # Elevated setup never writes into the user-controlled portable folder.
 # The normal host reads the protected journal and pins its bundled bridge.
 # No runtime JSON needs to be written by the administrator or a launch script.
 SaveProtected ($operation+'-ready.json') @{UserSid=$TargetUserSid;Controller=$state.ControllerInstance;SystemAction=($facts.SetupRequired);RebootRequired=$state.RebootRequired;IntendedRuntime=$bundle;RuntimeWrittenByAdministrator=$false}
 Write-Output ('Setup ready for '+$targetAccount+'. Start Vibeshine normally and select Vibe Speakers. Shared drivers remain unchanged.')
}finally{if($held){$mutex.ReleaseMutex()};$mutex.Dispose()}
