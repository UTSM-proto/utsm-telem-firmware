[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$launcher = Join-Path $PSScriptRoot 'start_live_telem.ps1'
if (-not (Test-Path -LiteralPath $launcher)) {
  throw "Launcher not found: $launcher"
}

$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
$quotedLauncher = '"' + $launcher + '"'
$action = New-ScheduledTaskAction -Execute $powershell -Argument (
  "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File $quotedLauncher -Background"
)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -RestartCount 5 `
  -RestartInterval (New-TimeSpan -Minutes 1) `
  -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME `
  -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName 'UTSM Live Telemetry Dashboard' `
  -Action $action -Trigger $trigger -Settings $settings `
  -Principal $principal -Force | Out-Null

Write-Host 'Installed UTSM dashboard auto-start for Windows sign-in.' `
  -ForegroundColor Green
