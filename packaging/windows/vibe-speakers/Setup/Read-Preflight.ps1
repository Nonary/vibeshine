[CmdletBinding()]
param([switch]$Json)
. (Join-Path $PSScriptRoot 'Common.ps1')
try{
 $facts=GetPreflight
 if($Json){$facts|ConvertTo-Json -Depth 12}else{
  Write-Output ('Windows '+$facts.WindowsBuild+' | HVCI '+$facts.HvciRunning+' | Secure Boot '+$facts.SecureBoot)
  Write-Output 'Private in-house hardware test only. Secure Boot hardware qualification is pending.'
  if($facts.SetupRequired){Write-Output 'Run Setup.cmd once and confirm administrator access.'}else{Write-Output 'Owned controller ready; streaming uses your normal account.'}
  foreach($errorText in $facts.Errors){Write-Output ('BLOCKED: '+$errorText)}
 }
 if(!$facts.Compatible){exit 1}
}catch{Write-Error $_;exit 1}
