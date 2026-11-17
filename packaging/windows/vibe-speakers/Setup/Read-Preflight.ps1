[CmdletBinding()]
param([switch]$Json)
. (Join-Path $PSScriptRoot 'Common.ps1')
try{
 $facts=GetPreflight
 if($Json){$facts|ConvertTo-Json -Depth 12}else{
  Write-Output ('Windows '+$facts.WindowsBuild+' | HVCI '+$facts.HvciRunning+' | Secure Boot '+$facts.SecureBoot)
  Write-Output 'Driver and device checks are read-only; Windows security settings are never changed.'
  if($facts.SetupRequired){Write-Output 'Open Vibeshine Audio settings and choose Set up Vibe Speakers.'}else{Write-Output 'Owned controller ready; streaming uses your normal account.'}
  foreach($errorText in $facts.Errors){Write-Output ('BLOCKED: '+$errorText)}
 }
 if(!$facts.Compatible){exit 1}
}catch{Write-Error $_;exit 1}
