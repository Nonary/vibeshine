[CmdletBinding()]
param([ValidateSet('Install','Remove','Preflight','Commit','Rollback')][string]$Action='Preflight',
      [Parameter(Mandatory)][string]$TargetUserSid,[switch]$ConfirmSetup,[ValidatePattern('^(?:\{[0-9A-Fa-f-]{36}\}|)$')][string]$InstallerTransaction,[string]$BootReceipt)
trap {
 Write-Error ($_.Exception.ToString()+"`n"+$_.ScriptStackTrace) -ErrorAction Continue
 exit 1
}
. (Join-Path $PSScriptRoot 'Common.ps1')
Require (IsAdmin) 'Administrator confirmation is required for this setup step'
if($Action -ne 'Preflight'){Require $ConfirmSetup 'Setup consent is missing'}
try{$targetAccount=([Security.Principal.SecurityIdentifier]::new($TargetUserSid)).Translate([Security.Principal.NTAccount]).Value}
catch{if($Action -ne 'Remove'){throw};$targetAccount=$TargetUserSid}
$stateDir=StateDirectory $TargetUserSid
function ProtectedAcl([bool]$directory){
 $acl=if($directory){[Security.AccessControl.DirectorySecurity]::new()}else{[Security.AccessControl.FileSecurity]::new()}
 $owner=if((CurrentSid) -eq 'S-1-5-18'){'S-1-5-18'}else{'S-1-5-32-544'}
 $acl.SetAccessRuleProtection($true,$false);$acl.SetOwner([Security.Principal.SecurityIdentifier]::new($owner))
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
function EnsureJournalDirectory {
 $base=Split-Path (Split-Path $stateDir)
 foreach($path in @($base,(Split-Path $stateDir),$stateDir)){
  if(!(Test-Path -LiteralPath $path)){$null=[IO.Directory]::CreateDirectory($path,(ProtectedAcl $true))}
  CheckJournalDirectory $path
  if($path -ine $stateDir){
   # Preserve the existing owner and group, including the shared namespace.
   $acl=[IO.Directory]::GetAccessControl($path,[Security.AccessControl.AccessControlSections]::Access)
   $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-11'),'ReadAndExecute','None','None','Allow'))
   [IO.Directory]::SetAccessControl($path,$acl)
  }
 }
}
function CheckOwnership($state){
 Require ($state.CreatedByVibeshine -is [bool] -and $state.CreatedByVibeshine -and $state.InstallRoot -ieq (PackageRoot)) 'Setup journal belongs to another installation'
 Require ($state.Token -cmatch '^[0-9a-f]{32}$' -and $state.TargetSddl -ceq [VibeSpeakers.Setup.UsbIpDeviceSecurity]::Sddl($TargetUserSid)) 'Setup journal ownership fields differ'
}
function MarkRemoved($state){
 $state.Phase='Removed'
 $state.RemovedUtc=[datetime]::UtcNow.ToString('o')
 SaveProtected 'setup.json' $state
}
function RemoveOwned($state){
 CheckOwnership $state
 Require ($state.Phase -in @('Ready','Installing')) 'Unexpected setup journal phase'
 if($state.ControllerInstance){
  $instance=[string]$state.ControllerInstance
  Require ($instance -match '^ROOT\\USB\\[0-9A-Fa-f]{4,32}$') 'Invalid owned controller instance'
  # The host holds this same exclusive named event while attaching the device.
  # Keep it reserved until the removal and its journal write are complete.
  $name='Global\VibeSpeakersController_'+$instance.ToUpperInvariant().Replace('\','_')
  $acl=[Security.AccessControl.EventWaitHandleSecurity]::new()
  foreach($sid in 'S-1-5-18','S-1-5-32-544'){
   $acl.AddAccessRule([Security.AccessControl.EventWaitHandleAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid),'FullControl','Allow'))
  }
  $created=$false
  $lease=[Threading.EventWaitHandle]::new($false,'ManualReset',$name,[ref]$created,$acl)
  try{
   Require $created 'Vibe Speakers controller is in use; close active streams before changing it'
   [VibeSpeakers.Setup.UsbControllerSetup]::Remove($instance,$state.Token,$state.TargetSddl)
   MarkRemoved $state
  }finally{$lease.Dispose()}
 }else{
  Require ($state.Phase -eq 'Installing') 'Ready controller instance is missing from the journal'
  MarkRemoved $state
 }
}

# Check the protected installed payload for privileged installer actions. Driver
# and firmware checks apply to provisioning, whereas removal checks device ownership.
$null=CheckPayload
LoadNative
if($Action -eq 'Preflight'){
 GetPreflight $TargetUserSid $BootReceipt -InspectOwned | ConvertTo-Json -Depth 12
 return
}
$mutex=[Threading.Mutex]::new($false,'Global\VibeshineUsbIpInstall')
$held=$false
$pending=$null
try{
 try{$held=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
 Require $held 'Another USB/IP setup is active'
 if(Test-Path -LiteralPath $stateDir){
  foreach($path in @((Split-Path (Split-Path $stateDir)),(Split-Path $stateDir),$stateDir)){CheckJournalDirectory $path}
 }
 $state=ReadState $TargetUserSid
 if($Action -eq 'Remove'){
  if($state -and $state.Phase -ne 'Removed'){RemoveOwned $state}
  return
 }
 if($Action -in @('Rollback','Commit')){
  if(!(OwnsInstallerTransaction $state (PackageRoot) $InstallerTransaction)){return}
  if($Action -eq 'Rollback'){RemoveOwned $state}
  elseif($state.Phase -eq 'Ready'){
   CheckOwnership $state
   Require ([bool][VibeSpeakers.Setup.UsbControllerSetup]::CheckMarker($state.ControllerInstance,$state.Token)) 'The provisioned controller disappeared before commit'
   $state.InstallerCommitted=$true
   SaveProtected 'setup.json' $state
  }
  return
 }

 if($InstallerTransaction){Require ([bool]$BootReceipt) 'Use the regular Vibeshine installer to verify the current boot policy'}
 $facts=GetPreflight $TargetUserSid $BootReceipt
 Require $facts.Compatible ($facts.Errors -join '; ')
 if($state -and $state.Phase -ne 'Removed'){
  CheckOwnership $state
  if($state.Phase -eq 'Ready' -and [VibeSpeakers.Setup.UsbControllerSetup]::CheckMarker($state.ControllerInstance,$state.Token)){
   [VibeSpeakers.Setup.UsbIpDeviceSecurity]::Bind($state.ControllerInstance)
   $snap=[VibeSpeakers.Setup.UsbIpDeviceSecurity]::InspectForRemoval()
   Require ([VibeSpeakers.Setup.UsbIpDeviceSecurity]::Equivalent($snap.Security,$state.TargetSddl)) 'Own controller ACL changed'
   if($snap.Started -and $snap.ProblemCode -eq 0 -and $state.InstallerCommitted -eq $true){
    Write-Output 'Reusing the installed Vibe Speakers controller.'
    return
   }
  }
  RemoveOwned $state
 }
 EnsureJournalDirectory
 $token=[guid]::NewGuid().ToString('N')
 $sddl=[VibeSpeakers.Setup.UsbIpDeviceSecurity]::Sddl($TargetUserSid)
 $pending=[pscustomobject]@{Schema='vibe-speakers-setup/1';UserSid=$TargetUserSid;UserAccount=$targetAccount;Token=$token;TargetSddl=$sddl;
  Phase='Installing';ControllerInstance='';CreatedUtc=[datetime]::UtcNow.ToString('o');RemovedUtc=$null;CreatedByVibeshine=$true;SharedDriversRetained=$true;
  SetupBoot=$facts.BootTime;RebootRequired=$false;InstallRoot=(PackageRoot);InstallerTransaction=$InstallerTransaction;InstallerCommitted=([string]::IsNullOrEmpty($InstallerTransaction))}
 SaveProtected 'setup.json' $pending
 $dep=Join-Path (PackageRoot) 'drivers\usbip-win2'
 foreach($name in 'usbip2_filter.inf','usbip2_ude.inf'){
  if(@($facts.StagedInf|Where-Object Original -eq $name).Count -eq 0){
   & "$env:SystemRoot\System32\pnputil.exe" /add-driver (Join-Path $dep $name)
   Require ($LASTEXITCODE -in @(0,3010)) ('Signed Driver Store staging failed: '+$name)
   if($LASTEXITCODE -eq 3010){$pending.RebootRequired=$true}
  }
 }
 $record=[Action[string]]{param($id) $pending.ControllerInstance=$id;SaveProtected 'setup.json' $pending}
 $created=[VibeSpeakers.Setup.UsbControllerSetup]::Create($token,$sddl,$record)
 $pending.ControllerInstance=$created.InstanceId
 $pending.RebootRequired=$pending.RebootRequired -or $created.RebootRequired
 $snap=[VibeSpeakers.Setup.UsbIpDeviceSecurity]::Inspect()
 Require ([VibeSpeakers.Setup.UsbIpDeviceSecurity]::Equivalent($snap.Security,$sddl)) 'New root ACL does not match the approved user'
 Require ($snap.Started -and $snap.ProblemCode -eq 0) 'New root did not start'
 $pending.Phase='Ready'
 SaveProtected 'setup.json' $pending
 if($pending.RebootRequired){Write-Output 'VIBE_SPEAKERS_RESTART_REQUIRED'}
 Write-Output 'Vibe Speakers controller is ready.'
}catch{
 $failure=$_
 if($pending){
  try{
   if(!$pending.ControllerInstance -or [VibeSpeakers.Setup.UsbControllerSetup]::LastCreateRolledBack){MarkRemoved $pending}
   else{RemoveOwned $pending}
  }catch{
   Write-Output ('VIBE_SPEAKERS_WARNING: Owned device cleanup could not complete: '+$_.Exception.Message)
  }
 }
 Write-Output ('VIBE_SPEAKERS_WARNING: '+$failure.Exception.Message)
 throw $failure
}finally{
 if($held){$mutex.ReleaseMutex()}
 $mutex.Dispose()
}
