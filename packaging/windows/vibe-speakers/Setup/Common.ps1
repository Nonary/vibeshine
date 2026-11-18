$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
function Require([bool]$value,[string]$message){if(!$value){throw $message}}
function IsAdmin {return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)}
function CurrentSid {return [Security.Principal.WindowsIdentity]::GetCurrent().User.Value}
function Hash([string]$file){return (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash}
function NoLinks([string]$file){
 $item=Get-Item -LiteralPath $file -Force
 while($item){Require (!(($item.Attributes -band [IO.FileAttributes]::ReparsePoint))) ('Reparse path refused: '+$item.FullName);if($item.PSIsContainer){$item=$item.Parent}else{$item=$item.Directory}}
}
function PackageRoot {return (Split-Path (Split-Path $PSScriptRoot))}
function StateDirectory([string]$sid){
 $parsed=[Security.Principal.SecurityIdentifier]::new($sid);Require ($parsed.IsAccountSid()) 'A real account SID is required'
 return (Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) ('VibeSpeakers\users\'+$parsed.Value))
}
function OwnsInstallerTransaction($state,[string]$root,[string]$transaction){
 return ($null -ne $state -and $state.CreatedByVibeshine -is [bool] -and $state.CreatedByVibeshine -and
  $state.InstallRoot -ieq $root -and $state.Phase -in @('Installing','Ready') -and
  $state.InstallerCommitted -is [bool] -and !$state.InstallerCommitted -and
  $transaction -match '^\{[0-9A-Fa-f-]{36}\}$' -and $state.InstallerTransaction -ieq $transaction)
}
function LoadNative {
 if(!('VibeSpeakers.Setup.UsbIpDeviceSecurity' -as [type])){
  Add-Type -Path @((Join-Path $PSScriptRoot 'UsbIpDeviceSecurity.cs'),(Join-Path $PSScriptRoot 'UsbControllerSetup.cs'))
 }
 if(!('VibeSpeakers.Setup.CodeIntegrity' -as [type])){Add-Type @'
using System;
using System.Runtime.InteropServices;
namespace VibeSpeakers.Setup {
 public static class CodeIntegrity {
  [StructLayout(LayoutKind.Sequential)] struct Info {public uint Size,Flags;}
  [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int c,ref Info i,uint bytes,out uint returned);
  public static uint Flags(){var i=new Info{Size=8};uint n;int s=NtQuerySystemInformation(103,ref i,8,out n);if(s<0||n!=8)throw new InvalidOperationException("Code Integrity query failed: "+s);return i.Flags;}
 }
}
'@}
}
function ReadState([string]$sid){
 $file=Join-Path (StateDirectory $sid) 'setup.json'
 if(!(Test-Path -LiteralPath $file)){return $null}
 NoLinks $file
 $acl=Get-Acl -LiteralPath $file
 Require ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -in @('S-1-5-18','S-1-5-32-544')) 'Setup state is not administrator-owned'
 foreach($ace in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
  if($ace.AccessControlType -eq 'Allow' -and $ace.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544')){
   $unsafe=[Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership -bor [Security.AccessControl.FileSystemRights]::Delete
   Require (($ace.FileSystemRights -band $unsafe) -eq 0) 'Setup state has untrusted write permissions'
  }
 }
 $value=Get-Content -LiteralPath $file -Raw|ConvertFrom-Json
 Require ($value.Schema -ceq 'vibe-speakers-setup/1' -and $value.UserSid -ceq $sid) 'Setup journal belongs to another user or version'
 return $value
}
function CheckPayload {
 $root=PackageRoot;NoLinks $root
 Require ([IO.Path]::GetPathRoot($root) -match '^[A-Za-z]:\\$') 'Install Vibeshine to a local NTFS drive, not a network path'
 $drive=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($root))
 Require ([string]$drive.DriveType -in @('Fixed','Removable') -and $drive.DriveFormat -eq 'NTFS') 'A local NTFS folder is required for the controlled access permissions'
 if(Test-Path -LiteralPath (Join-Path $root 'VibeSpeakers\installed-payload.json')){
  foreach($relative in @('sunshine.exe','tools\VibeUsbBridge.exe') + @(Get-ChildItem -LiteralPath $PSScriptRoot -File | ForEach-Object {$_.FullName.Substring($root.Length+1)})){
   $path=Join-Path $root $relative;NoLinks $path
   $chain=@();$candidate=$path
   while($true){
    $chain+=,$candidate
    if($candidate -ieq $root){break}
    $parent=Split-Path $candidate
    Require ($parent -and $parent -ne $candidate -and $candidate.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)) 'Payload escaped its installation'
    $candidate=$parent
   }
   foreach($candidate in $chain){
    $acl=Get-Acl -LiteralPath $candidate
    Require ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -in @('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')) 'Installed payload must be administrator-owned'
    foreach($ace in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
     if($ace.AccessControlType -eq 'Allow' -and !($ace.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -and $ace.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')){
      $writes=[long][Security.AccessControl.FileSystemRights]::Write -bor [long][Security.AccessControl.FileSystemRights]::Delete -bor [long][Security.AccessControl.FileSystemRights]::ChangePermissions -bor [long][Security.AccessControl.FileSystemRights]::TakeOwnership -bor 0x10000000L -bor 0x40000000L -bor 0x40L
      Require (([long]$ace.FileSystemRights -band $writes) -eq 0) 'Installed payload is writable by an untrusted principal'
     }
    }
   }
  }
  return @{Schema='vibe-speakers-installed-payload/1';BridgeSHA256=(Hash (Join-Path $root 'tools\VibeUsbBridge.exe'))}
 }
 throw 'Installed Vibe Speakers payload marker is missing; repair the Vibeshine installation'
}

function CheckDriverPackage {
 $dir=Join-Path (PackageRoot) 'drivers\usbip-win2'
 $lock=Get-Content (Join-Path $dir 'release-lock.json') -Raw|ConvertFrom-Json
 Require ($lock.version -ceq '0.9.8.1' -and $lock.installer_sha256 -ieq '38CAD6D4432B52D5BB9409D9AD03B72FDFFC4ADA4CD3A48FBECA1A2752A8518A') 'Wrong USB/IP package version'
 foreach($p in $lock.files.PSObject.Properties){Require ((Hash (Join-Path $dir $p.Name)) -ieq $p.Value) ('Signed package hash differs: '+$p.Name)}
 foreach($name in 'usbip2_ude.cat','usbip2_filter.cat','usbip2_ude.sys','usbip2_filter.sys'){
  $signature=Get-AuthenticodeSignature -LiteralPath (Join-Path $dir $name)
  Require ($signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match 'CN=Microsoft Windows Hardware Compatibility Publisher(,|$)') ('Microsoft signature invalid: '+$name)
 }
 return $lock
}
function DriverPath([string]$path){
 $path=$path.Trim('"')
 if($path.StartsWith('\SystemRoot\',[StringComparison]::OrdinalIgnoreCase)){$path=Join-Path $env:SystemRoot $path.Substring(12)}
 Require ([IO.Path]::IsPathRooted($path) -and $path -notlike '\\*') 'Driver is not a local absolute image'
 return $path
}
function CompatibilityErrors($facts){
 $errors=[Collections.Generic.List[string]]::new()
 if(!$facts.Windows11X64){$errors.Add('Windows 11 x64 required')}
 if(($facts.CodeIntegrityOptions -band 1) -eq 0 -or ($facts.CodeIntegrityOptions -band 2) -ne 0){$errors.Add('Code Integrity must be active and TESTSIGNING off')}
 if($facts.SecureBoot -notin @(0,1)){$errors.Add('Secure Boot state is unknown')}
 if($facts.AdminBootChecked -and ($facts.TestSigning -or $facts.NoIntegrityChecks)){$errors.Add('TESTSIGNING/nointegritychecks must be off')}
 foreach($e in $facts.DriverConflicts){$errors.Add([string]$e)}
 foreach($e in $facts.UsbIdentityConflicts){$errors.Add([string]$e)}
 return ,$errors.ToArray()
}
function IsOwnedDescendant([string]$id,[string]$root){
 if(!$root){return $false}
 $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 for($n=0;$n -lt 32 -and $id -and $seen.Add($id);$n++){
  if($id -ieq $root){return $true}
  $property=Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue
  $id=if($property){[string]$property.Data}else{''}
 }
 return $false
}
function ValidateBootObservation($value,[string]$boot,[int]$secureBoot,[datetime]$now){
 Require ($value.Schema -ceq 'vibe-speakers-installer-security/1' -and $value.SecureBoot -is [bool]) 'Invalid installer security observation'
 Require ($value.BootTime -ceq $boot) 'Installer security observation belongs to another Windows start'
 $age=($now.ToUniversalTime()-[datetime]::Parse($value.ObservedUtc).ToUniversalTime()).TotalSeconds
 Require ($age -ge 0 -and $age -le 1200) 'Installer security observation is stale or from the future'
 Require ([int]$value.SecureBoot -eq $secureBoot) 'Observed firmware and current Windows Secure Boot state disagree'
 Require ($value.TestSigning -is [bool] -and !$value.TestSigning -and $value.NoIntegrityChecks -is [bool] -and !$value.NoIntegrityChecks) 'Installer boot policy observation is unsafe or invalid'
}
function ReadBootObservation([string]$file,[string]$boot,[int]$secureBoot){
 $full=[IO.Path]::GetFullPath($file)
 $temp=[IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'Temp'))
 Require ($full -ceq $file -and (Split-Path $full -Leaf) -ceq 'vibe-boot.json') 'Noncanonical installer security observation'
 $parent=Split-Path $full
 Require ((Split-Path $parent) -ieq $temp -and (Split-Path $parent -Leaf) -match '^VibeshineInstallerRecovery_[0-9a-f]{32}$') 'Security observation is outside the protected installer namespace'
 NoLinks $full
 foreach($path in @($parent,$full)){
  $acl=Get-Acl -LiteralPath $path
  Require ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -in @('S-1-5-18','S-1-5-32-544')) 'Security observation is not administrator-owned'
  foreach($ace in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
   if($ace.AccessControlType -eq 'Allow' -and $ace.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544')){
    $writes=0x500D0156L
    Require (([long]$ace.FileSystemRights -band $writes) -eq 0) 'Security observation has untrusted writers'
   }
  }
 }
 $stream=[IO.FileStream]::new($full,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 try{
  Require ($stream.Length -gt 0 -and $stream.Length -le 8192) 'Security observation size is invalid'
  $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
  $value=$reader.ReadToEnd()|ConvertFrom-Json
  ValidateBootObservation $value $boot $secureBoot ([datetime]::UtcNow)
  return $value
 }finally{$stream.Dispose()}
}
function GetPreflight([string]$sid=(CurrentSid),[string]$bootReceipt,[switch]$InspectOwned){
 $null=CheckPayload;$lock=CheckDriverPackage;LoadNative
 $os=Get-CimInstance Win32_OperatingSystem
 $guard=Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard
 $sb=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled -ErrorAction Stop).UEFISecureBootEnabled
 $facts=[ordered]@{Schema='vibe-speakers-preflight/1';Utc=[datetime]::UtcNow.ToString('o');UserSid=$sid;Elevated=(IsAdmin);
  Windows11X64=([Environment]::Is64BitProcess -and [Environment]::Is64BitOperatingSystem -and $env:PROCESSOR_ARCHITECTURE -eq 'AMD64' -and [int]$os.BuildNumber -ge 22000);
  WindowsBuild=$os.BuildNumber;BootTime=$os.LastBootUpTime.ToUniversalTime().ToString('o');CodeIntegrityOptions=[VibeSpeakers.Setup.CodeIntegrity]::Flags();
  VbsStatus=[int]$guard.VirtualizationBasedSecurityStatus;HvciRunning=(2 -in $guard.SecurityServicesRunning);SecureBoot=[int]$sb;
  SecureBootHardwareQualification=$false;SecureBootFirmwareChecked=$false;AdminBootChecked=$false;TestSigning=$null;NoIntegrityChecks=$null;
  DriverConflicts=@();UsbIdentityConflicts=@();InstalledDrivers=@();StagedInf=@();AudioServices=@();OwnController=$null;SetupRequired=$true;SystemActions=$false}
 foreach($name in 'Audiosrv','AudioEndpointBuilder'){
  $service=Get-Service $name -ErrorAction Stop;$facts.AudioServices+=@{Name=$name;State=$service.Status.ToString()}
  if($service.Status -ne 'Running'){$facts.DriverConflicts+=('Required Windows audio service '+$name+' is not running; no service setting is changed')}
 }
 if(IsAdmin){
  if($bootReceipt){
   $observation=ReadBootObservation $bootReceipt $facts.BootTime $sb
   $facts.TestSigning=$observation.TestSigning;$facts.NoIntegrityChecks=$observation.NoIntegrityChecks;$facts.AdminBootChecked=$true
  }
  else{
   $firmware=Confirm-SecureBootUEFI -ErrorAction Stop
   Require ([int]$firmware -eq [int]$sb) 'Firmware and Windows Secure Boot state disagree'
  }
  $facts.SecureBootFirmwareChecked=$true
  if(!$bootReceipt){
  $boot=@(& "$env:SystemRoot\System32\bcdedit.exe" /enum '{current}' 2>&1);Require ($LASTEXITCODE -eq 0) 'Cannot read current boot loader'
  $facts.AdminBootChecked=$true
  foreach($pair in @(@('testsigning','TestSigning'),@('nointegritychecks','NoIntegrityChecks'))){
   $line=@($boot|Where-Object {$_ -match ('^\s*'+$pair[0]+'\s+')})
   if($line.Count){Require ($line.Count -eq 1 -and $line[0] -match ('^\s*'+$pair[0]+'\s+(Yes|Ja|On|1|No|Nein|Off|0)\s*$')) 'Unrecognized boot Boolean; setup refuses an ambiguous security state';$facts[$pair[1]]=($Matches[1] -in @('Yes','Ja','On','1'))}else{$facts[$pair[1]]=$false}
  }
  }
 }
 foreach($name in 'usbip2_ude','usbip2_filter'){
  $driver=Get-CimInstance Win32_SystemDriver -Filter "Name='$name'"
  if($driver){
   $file=DriverPath $driver.PathName;$sig=Get-AuthenticodeSignature -LiteralPath $file
   $facts.InstalledDrivers+=@{Name=$name;State=$driver.State;Path=$file;SHA256=(Hash $file);Signature=$sig.Status.ToString()}
   if((Hash $file) -ine $lock.files.($name+'.sys') -or $sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows Hardware Compatibility Publisher(,|$)'){$facts.DriverConflicts+=('Existing '+$name+' differs; it will not be replaced')}
  }
 }
 foreach($inf in Get-ChildItem -LiteralPath (Join-Path $env:SystemRoot 'INF') -Filter 'oem*.inf'){
  $match=Select-String -LiteralPath $inf.FullName -Pattern '^\s*CatalogFile\s*=\s*(usbip2_(?:ude|filter))\.cat\s*$'
  if($match){$original=$match[0].Matches[0].Groups[1].Value+'.inf';$facts.StagedInf+=@{Original=$original;Published=$inf.Name;SHA256=(Hash $inf.FullName)}
   if((Hash $inf.FullName) -ine $lock.files.$original){$facts.DriverConflicts+=('Different staged USB/IP package '+$inf.Name+'; automatic replacement refused')}
  }
 }
 $builtin=Join-Path $env:SystemRoot 'System32\drivers\usbaudio2.sys'
 Require ((Test-Path -LiteralPath $builtin) -and (Get-AuthenticodeSignature -LiteralPath $builtin).Status -eq 'Valid') 'Signed Windows USB Audio 2 driver unavailable'
 $state=ReadState $sid
 if($InspectOwned -and $state -and $state.Phase -eq 'Ready'){
  [VibeSpeakers.Setup.UsbIpDeviceSecurity]::Bind($state.ControllerInstance)
  $null=[VibeSpeakers.Setup.UsbControllerSetup]::CheckMarker($state.ControllerInstance,$state.Token)
  $snap=[VibeSpeakers.Setup.UsbIpDeviceSecurity]::Inspect()
  Require ([VibeSpeakers.Setup.UsbIpDeviceSecurity]::Equivalent($snap.Security,$state.TargetSddl)) 'Own controller ACL changed'
  Require ($snap.Started -and $snap.ProblemCode -eq 0) 'Own controller is not ready; inspect setup journal'
  $facts.OwnController=$snap;$facts.SetupRequired=$false
 }
 foreach($device in Get-PnpDevice -PresentOnly){
  if($device.InstanceId -match '^USB\\VID_1209&PID_(7140|00(0[1-9A-F]|10))(?:&|\\)' -and !(IsOwnedDescendant $device.InstanceId $(if($state){$state.ControllerInstance}else{''}))){$facts.UsbIdentityConflicts+=('Reserved test VID/PID already used by an unrelated device: '+$device.InstanceId)}
 }
 $facts.Errors=CompatibilityErrors ([pscustomobject]$facts)
 $facts.Compatible=($facts.Errors.Count -eq 0)
 return [pscustomobject]$facts
}
