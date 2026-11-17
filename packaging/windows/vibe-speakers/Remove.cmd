@echo off
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Setup\Confirm-Setup.ps1" -Action Remove
