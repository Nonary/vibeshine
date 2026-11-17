[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot 'Common.ps1')
Add-Type -AssemblyName PresentationFramework
try {
 $sid=CurrentSid
 $message='Set up Vibe Speakers for this Windows account? This creates one owned virtual audio controller. Existing shared USB/IP drivers are reused. Windows will request administrator approval. No Windows security settings are changed.'
 if([Windows.MessageBox]::Show($message,'Vibe Speakers','OKCancel','Information') -ne 'OK'){exit 0}
 $script=Join-Path $PSScriptRoot 'Invoke-AdminSetup.ps1'
 $arguments='-NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File "'+$script+'" -Action Install -TargetUserSid '+$sid+' -ConfirmSetup -ShowErrorDialog'
 $p=Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
 if($p.ExitCode -ne 0){throw ('Setup failed (exit '+$p.ExitCode+'). Check the Vibe Speakers status in Audio settings.')}
 [Windows.MessageBox]::Show('Vibe Speakers is ready. Select it in Audio settings and save.','Vibe Speakers','OK','Information')|Out-Null
} catch {
 [Windows.MessageBox]::Show($_.Exception.Message,'Vibe Speakers setup','OK','Error')|Out-Null
 exit 1
}
