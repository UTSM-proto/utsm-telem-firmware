[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Find-CommandPath([string[]]$Names, [string[]]$FallbackPaths) {
  foreach ($name in $Names) {
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($command) {
      return $command.Source
    }
  }
  foreach ($path in $FallbackPaths) {
    if (Test-Path -LiteralPath $path) {
      return $path
    }
  }
  throw "Required program not found: $($Names -join ', ')"
}

function Get-CharSetting([string]$Text, [string]$Name) {
  $escapedName = [regex]::Escape($Name)
  $pattern = 'static\s+const\s+char\s+' + $escapedName +
    '\s*\[\]\s*=\s*"(?<value>[^"]*)"\s*;'
  $match = [regex]::Match($Text, $pattern)
  if (-not $match.Success) {
    throw "Could not find $Name in relay_config.h"
  }
  return $match.Groups['value'].Value
}

function Set-CharSetting([string]$Text, [string]$Name, [string]$Value) {
  $escapedName = [regex]::Escape($Name)
  $pattern = '(?<prefix>static\s+const\s+char\s+' + $escapedName +
    '\s*\[\]\s*=\s*)"[^"]*"(?<suffix>\s*;)'
  if (-not [regex]::IsMatch($Text, $pattern)) {
    throw "Could not update $Name in relay_config.h"
  }
  return [regex]::Replace(
    $Text,
    $pattern,
    { param($match)
      $match.Groups['prefix'].Value + '"' + $Value + '"' +
        $match.Groups['suffix'].Value
    }
  )
}

function New-LocalApiKey {
  $bytes = New-Object byte[] 24
  $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
  try {
    $generator.GetBytes($bytes)
  } finally {
    $generator.Dispose()
  }
  return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

$firmwareRepo = $PSScriptRoot
$utsmRoot = [IO.Path]::GetFullPath((Join-Path $firmwareRepo '..\..\..'))
$softwareRepo = Join-Path $utsmRoot `
  '04_Telemetry-and-Track\Telemetry\utsm-proto-telemetry-host'
$relayConfig = Join-Path $firmwareRepo 'lte_relay\relay_config.h'
$relayConfigExample = Join-Path $firmwareRepo 'lte_relay\relay_config.example.h'
$stateDirectory = Join-Path $utsmRoot '.utsm-live'

if (-not (Test-Path -LiteralPath $softwareRepo)) {
  throw "Telemetry host not found: $softwareRepo"
}
if (-not (Test-Path -LiteralPath $relayConfig)) {
  Copy-Item -LiteralPath $relayConfigExample -Destination $relayConfig
}

$configText = [IO.File]::ReadAllText($relayConfig)
$apiKey = Get-CharSetting $configText 'TELEMETRY_API_KEY'
if (-not $apiKey -or $apiKey -match 'change-me|replace|YOUR_') {
  $apiKey = New-LocalApiKey
  $configText = Set-CharSetting $configText 'TELEMETRY_API_KEY' $apiKey
  $utf8NoBom = New-Object Text.UTF8Encoding($false)
  [IO.File]::WriteAllText($relayConfig, $configText, $utf8NoBom)
}

$python = Join-Path $softwareRepo '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) {
  $systemPython = Find-CommandPath @('python') @()
  & $systemPython -m venv (Join-Path $softwareRepo '.venv')
  if ($LASTEXITCODE -ne 0) {
    throw 'Failed to create the telemetry Python environment.'
  }
}

& $python -c 'import fastapi, uvicorn, serial' 2>$null
if ($LASTEXITCODE -ne 0) {
  & $python -m pip install -r (Join-Path $softwareRepo 'requirements.txt')
  if ($LASTEXITCODE -ne 0) {
    throw 'Failed to install the telemetry dashboard dependencies.'
  }
}

$listeners = @(
  Get-NetTCPConnection -LocalPort 8000 -State Listen -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty OwningProcess -Unique
)
if ($listeners.Count -gt 0) {
  if ($listeners.Count -ne 1) {
    throw 'More than one process is listening on port 8000; refusing to stop them.'
  }

  $listenerPid = [int]$listeners[0]
  $listenerProcess = Get-CimInstance Win32_Process `
    -Filter "ProcessId=$listenerPid"
  $isUtsmDashboard = $listenerProcess -and
    $listenerProcess.Name -eq 'python.exe' -and
    $listenerProcess.CommandLine -match `
      '-m\s+uvicorn\s+live_dashboard\.app:app' -and
    $listenerProcess.CommandLine -match '--port\s+8000'
  if (-not $isUtsmDashboard) {
    throw "Port 8000 belongs to another program (PID $listenerPid); refusing to stop it."
  }

  try {
    $setupStatus = Invoke-RestMethod `
      -Uri 'http://127.0.0.1:8000/api/setup/status' -TimeoutSec 2
    if ($setupStatus.running) {
      throw 'The WROVER is currently being prepared or programmed. Wait for it to finish.'
    }
  } catch {
    if ($_.Exception.Message -like 'The WROVER is currently*') {
      throw
    }
  }

  Write-Host "Closing the old UTSM telemetry dashboard (PID $listenerPid)..."
  Stop-Process -Id $listenerPid -ErrorAction Stop
  for ($attempt = 0; $attempt -lt 20; $attempt++) {
    Start-Sleep -Milliseconds 250
    $stillListening = Get-NetTCPConnection -LocalPort 8000 -State Listen `
      -ErrorAction SilentlyContinue
    if (-not $stillListening) {
      break
    }
  }
  if (Get-NetTCPConnection -LocalPort 8000 -State Listen `
      -ErrorAction SilentlyContinue) {
    throw 'The old UTSM telemetry dashboard did not release port 8000.'
  }
}

New-Item -ItemType Directory -Force -Path $stateDirectory | Out-Null
$env:UTSM_TELEMETRY_API_KEY = $apiKey
$env:UTSM_SETUP_ENABLED = '1'
$env:UTSM_FIRMWARE_REPO = $firmwareRepo
$env:UTSM_SOFTWARE_REPO = $softwareRepo

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stdoutLog = Join-Path $stateDirectory "portal-$stamp.out.log"
$stderrLog = Join-Path $stateDirectory "portal-$stamp.err.log"
$portalProcess = Start-Process -FilePath $python -ArgumentList @(
  '-m', 'uvicorn', 'live_dashboard.app:app',
  '--host', '127.0.0.1', '--port', '8000'
) -WorkingDirectory $softwareRepo -WindowStyle Hidden `
  -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog `
  -PassThru

[IO.File]::WriteAllText(
  (Join-Path $stateDirectory 'dashboard.pid'),
  [string]$portalProcess.Id
)

$ready = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
  Start-Sleep -Milliseconds 500
  if ($portalProcess.HasExited) {
    break
  }
  try {
    $status = Invoke-RestMethod -Uri 'http://127.0.0.1:8000/api/setup/status' `
      -TimeoutSec 2
    if ($status.enabled) {
      $ready = $true
      break
    }
  } catch {
    # The server is still starting.
  }
}

if (-not $ready) {
  if (-not $portalProcess.HasExited) {
    Stop-Process -Id $portalProcess.Id
  }
  throw "The setup portal failed to start. Inspect $stderrLog"
}

Start-Process 'http://127.0.0.1:8000/setup'
Write-Host ''
Write-Host 'UTSM live telemetry setup is ready.' -ForegroundColor Green
Write-Host 'The setup page opened at http://127.0.0.1:8000/setup'
Write-Host 'Connect only the WROVER, choose its COM port, and click Program.'
Write-Host 'The dashboard and tunnel will keep running in the background.'
