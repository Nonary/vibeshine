@echo off
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Setup\Read-Preflight.ps1"
