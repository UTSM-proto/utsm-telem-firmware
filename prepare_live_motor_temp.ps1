[CmdletBinding()]
param(
  [switch]$NoLaunchIde,
  [switch]$ForceNewTunnel
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$Message) {
  Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Find-Executable([string[]]$Names, [string[]]$FallbackPaths) {
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

function Test-Health([string]$Url, [int]$TimeoutSec = 5) {
  try {
    $response = Invoke-RestMethod -Uri $Url -TimeoutSec $TimeoutSec
    return [bool]$response.ok
  } catch {
    return $false
  }
}

function Read-SharedText([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) {
    return ''
  }

  $stream = New-Object IO.FileStream(
    $Path,
    [IO.FileMode]::Open,
    [IO.FileAccess]::Read,
    [IO.FileShare]::ReadWrite
  )
  $reader = New-Object IO.StreamReader($stream)
  try {
    return $reader.ReadToEnd()
  } finally {
    $reader.Dispose()
    $stream.Dispose()
  }
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
$relaySketch = Join-Path $firmwareRepo 'lte_relay\lte_relay.ino'
$c3Sketch = Join-Path $firmwareRepo `
  'telem-v2\telemetry_v2\telemetry_v2.ino'
$relayConfig = Join-Path $firmwareRepo 'lte_relay\relay_config.h'
$relayConfigExample = Join-Path $firmwareRepo `
  'lte_relay\relay_config.example.h'
$stateDirectory = Join-Path $utsmRoot '.utsm-live'

foreach ($requiredPath in @($softwareRepo, $relaySketch, $c3Sketch)) {
  if (-not (Test-Path -LiteralPath $requiredPath)) {
    throw "Required UTSM path not found: $requiredPath"
  }
}

$arduinoCli = Find-Executable @('arduino-cli') @(
  'C:\Program Files\Arduino CLI\arduino-cli.exe'
)
$arduinoIde = Find-Executable @('arduino-ide') @(
  'C:\Program Files\Arduino IDE\Arduino IDE.exe'
)
$cloudflared = Find-Executable @('cloudflared') @(
  'C:\Program Files (x86)\cloudflared\cloudflared.exe'
)
$python = Find-Executable @('python') @()
$git = Find-Executable @('git') @()

Write-Step 'Checking the motor-temperature firmware checkout'
$packetHeader = Get-Content (Join-Path $firmwareRepo `
  'telem-v2\telemetry_v2\live_telemetry_packet.h') -Raw
if ($packetHeader -notmatch 'LIVE_TELEMETRY_VERSION\s*=\s*4' -or
    $packetHeader -notmatch 'motor_temperature_c_x100') {
  throw 'This checkout does not contain the packet-v4 motor-temperature change.'
}

Write-Step 'Installing the Arduino ESP32 platform and sensor library'
$coreList = (& $arduinoCli core list 2>&1 | Out-String)
if ($coreList -notmatch '(?m)^esp32:esp32\s+3\.3\.11\s') {
  & $arduinoCli core install 'esp32:esp32@3.3.11'
  if ($LASTEXITCODE -ne 0) {
    throw 'Failed to install esp32:esp32@3.3.11.'
  }
}

$libraryList = (& $arduinoCli lib list 2>&1 | Out-String)
if ($libraryList -notmatch '(?im)^TinyGPSPlus\s') {
  & $arduinoCli lib install TinyGPSPlus
  if ($LASTEXITCODE -ne 0) {
    throw 'Failed to install TinyGPSPlus.'
  }
}

Write-Step 'Installing LilyGO TinyGSM support for the A7670 relay'
$lilyGoRepo = Join-Path $utsmRoot 'tmp\lilygo-modem-series'
$lilyGoTinyGsm = Join-Path $lilyGoRepo 'lib\TinyGSM'
if (-not (Test-Path -LiteralPath (Join-Path $lilyGoTinyGsm `
    'src\TinyGsmClient.h'))) {
  New-Item -ItemType Directory -Force -Path (Split-Path $lilyGoRepo -Parent) |
    Out-Null
  & $git clone --depth 1 `
    'https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series.git' `
    $lilyGoRepo
  if ($LASTEXITCODE -ne 0) {
    throw 'Failed to download LilyGO Modem Series.'
  }
}

$arduinoUserOutput = @(& $arduinoCli config get directories.user) |
  ForEach-Object { [string]$_ } |
  Where-Object { $_.Trim() }
$arduinoUserDirectory = ''
if ($arduinoUserOutput) {
  $arduinoUserDirectory = ([string]($arduinoUserOutput | Select-Object -Last 1)).Trim()
}
if (-not $arduinoUserDirectory) {
  $oneDriveSketchbook = Join-Path $env:USERPROFILE 'OneDrive\Documents\Arduino'
  $localSketchbook = Join-Path $env:USERPROFILE 'Documents\Arduino'
  if (Test-Path -LiteralPath $oneDriveSketchbook) {
    $arduinoUserDirectory = $oneDriveSketchbook
  } elseif (Test-Path -LiteralPath $localSketchbook) {
    $arduinoUserDirectory = $localSketchbook
  }
}
if (-not $arduinoUserDirectory) {
  throw 'Arduino sketchbook directory is not configured.'
}
$arduinoLibraries = Join-Path $arduinoUserDirectory 'libraries'
$installedTinyGsm = Join-Path $arduinoLibraries 'TinyGSM'
$installedTinyGsmHeader = Join-Path $installedTinyGsm 'src\TinyGsmClient.h'
$tinyGsmReady = (Test-Path -LiteralPath $installedTinyGsmHeader) -and
  (Select-String -Path $installedTinyGsmHeader `
    -Pattern 'TINY_GSM_MODEM_A7670' -Quiet)

if (-not $tinyGsmReady) {
  New-Item -ItemType Directory -Force -Path $arduinoLibraries | Out-Null
  if (Test-Path -LiteralPath $installedTinyGsm) {
    $backupRoot = Join-Path $utsmRoot 'tmp\arduino-library-backups'
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    $backupName = 'TinyGSM-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
    Move-Item -LiteralPath $installedTinyGsm `
      -Destination (Join-Path $backupRoot $backupName)
    Write-Host 'Backed up the incompatible TinyGSM library.'
  }
  Copy-Item -LiteralPath $lilyGoTinyGsm -Destination $installedTinyGsm `
    -Recurse
}

if (-not (Select-String -Path $installedTinyGsmHeader `
    -Pattern 'TINY_GSM_MODEM_A7670' -Quiet)) {
  throw 'The installed TinyGSM library still lacks A7670 support.'
}

Write-Step 'Preparing the ignored relay configuration'
if (-not (Test-Path -LiteralPath $relayConfig)) {
  Copy-Item -LiteralPath $relayConfigExample -Destination $relayConfig
}

$configText = [IO.File]::ReadAllText($relayConfig)
$apiKey = Get-CharSetting $configText 'TELEMETRY_API_KEY'
if (-not $apiKey -or $apiKey -match 'change-me|replace|YOUR_') {
  $apiKey = New-LocalApiKey
  $configText = Set-CharSetting $configText 'TELEMETRY_API_KEY' $apiKey
  Write-Host 'Generated a local telemetry API key (not displayed).'
} else {
  Write-Host 'Using the existing telemetry API key (not displayed).'
}

$apn = Get-CharSetting $configText 'LTE_APN'
if (-not $apn -or $apn -match 'YOUR_|replace') {
  throw 'LTE_APN is not configured in lte_relay\relay_config.h.'
}

if ($configText -match 'LTE_DUMMY_TEST_MODE\s*=\s*true') {
  $configText = $configText -replace
    '(LTE_DUMMY_TEST_MODE\s*=\s*)true', '${1}false'
}

New-Item -ItemType Directory -Force -Path $stateDirectory | Out-Null

Write-Step 'Starting the local live dashboard'
& $python -c 'import fastapi, uvicorn' 2>$null
if ($LASTEXITCODE -ne 0) {
  & $python -m pip install -r (Join-Path $softwareRepo 'requirements.txt')
  if ($LASTEXITCODE -ne 0) {
    throw 'Failed to install the dashboard Python dependencies.'
  }
}

if (-not (Test-Health 'http://127.0.0.1:8000/health' 2)) {
  $env:UTSM_TELEMETRY_API_KEY = $apiKey
  $dashboardStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $dashboardOut = Join-Path $stateDirectory `
    "dashboard-$dashboardStamp.out.log"
  $dashboardErr = Join-Path $stateDirectory `
    "dashboard-$dashboardStamp.err.log"
  $dashboardProcess = Start-Process -FilePath $python -ArgumentList @(
    '-m', 'uvicorn', 'live_dashboard.app:app',
    '--host', '127.0.0.1', '--port', '8000'
  ) -WorkingDirectory $softwareRepo -WindowStyle Hidden `
    -RedirectStandardOutput $dashboardOut `
    -RedirectStandardError $dashboardErr -PassThru
  [IO.File]::WriteAllText(
    (Join-Path $stateDirectory 'dashboard.pid'),
    [string]$dashboardProcess.Id
  )

  $dashboardReady = $false
  for ($attempt = 0; $attempt -lt 20; $attempt++) {
    Start-Sleep -Seconds 1
    if (Test-Health 'http://127.0.0.1:8000/health' 2) {
      $dashboardReady = $true
      break
    }
  }
  if (-not $dashboardReady) {
    throw "Dashboard failed to start. Inspect $dashboardErr"
  }
}
Write-Host 'Dashboard ready at http://127.0.0.1:8000/live'

Write-Step 'Checking or creating the public Cloudflare tunnel'
$currentEndpoint = Get-CharSetting $configText 'TELEMETRY_ENDPOINT'
$currentHealthUrl = $currentEndpoint -replace
  '/api/live/telemetry$', '/health'
$publicBaseUrl = $null

if (-not $ForceNewTunnel -and $currentEndpoint -match
    '^https://[a-z0-9-]+\.trycloudflare\.com/api/live/telemetry$' -and
    (Test-Health $currentHealthUrl 10)) {
  $publicBaseUrl = $currentEndpoint -replace '/api/live/telemetry$', ''
  Write-Host 'Existing public tunnel is healthy.'
} else {
  $lastTunnelErrorLog = $null
  for ($tunnelTry = 1; $tunnelTry -le 3 -and -not $publicBaseUrl;
      $tunnelTry++) {
    Write-Host "Starting quick tunnel attempt $tunnelTry of 3..."
    $tunnelStamp = (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$tunnelTry"
    $tunnelOut = Join-Path $stateDirectory "tunnel-$tunnelStamp.out.log"
    $tunnelErr = Join-Path $stateDirectory "tunnel-$tunnelStamp.err.log"
    $lastTunnelErrorLog = $tunnelErr
    $tunnelProcess = Start-Process -FilePath $cloudflared -ArgumentList @(
      'tunnel', '--url', 'http://127.0.0.1:8000', '--no-autoupdate'
    ) -WindowStyle Hidden -RedirectStandardOutput $tunnelOut `
      -RedirectStandardError $tunnelErr -PassThru

    $candidateBaseUrl = $null
    for ($attempt = 0; $attempt -lt 45; $attempt++) {
      Start-Sleep -Seconds 1
      $tunnelLog = ''
      $tunnelLog += Read-SharedText $tunnelOut
      $tunnelLog += Read-SharedText $tunnelErr
      $urlMatch = [regex]::Match(
        $tunnelLog,
        'https://[a-z0-9-]+\.trycloudflare\.com'
      )
      if ($urlMatch.Success) {
        $candidateBaseUrl = $urlMatch.Value
        break
      }
      if ($tunnelProcess.HasExited) {
        break
      }
    }

    if ($candidateBaseUrl) {
      for ($attempt = 0; $attempt -lt 20; $attempt++) {
        if (Test-Health "$candidateBaseUrl/health" 5) {
          $publicBaseUrl = $candidateBaseUrl
          break
        }
        Start-Sleep -Seconds 1
      }
    }

    if ($publicBaseUrl) {
      [IO.File]::WriteAllText(
        (Join-Path $stateDirectory 'tunnel.pid'),
        [string]$tunnelProcess.Id
      )
      Write-Host 'New public tunnel is healthy.'
    } else {
      Write-Warning 'Tunnel hostname did not become publicly reachable.'
      if (-not $tunnelProcess.HasExited) {
        Stop-Process -Id $tunnelProcess.Id
        $tunnelProcess.WaitForExit(5000) | Out-Null
      }
    }
  }

  if (-not $publicBaseUrl) {
    throw "Cloudflare failed after three attempts. Inspect $lastTunnelErrorLog"
  }
}

$newEndpoint = "$publicBaseUrl/api/live/telemetry"
if ($currentEndpoint -ne $newEndpoint) {
  $configText = Set-CharSetting $configText 'TELEMETRY_ENDPOINT' $newEndpoint
  Write-Host 'Updated the ignored relay endpoint for this tunnel.'
}

$utf8NoBom = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText($relayConfig, $configText, $utf8NoBom)

if (-not $NoLaunchIde) {
  Write-Step 'Opening both Arduino sketches'
  Start-Process -FilePath $arduinoIde -ArgumentList @("`"$c3Sketch`"")
  Start-Sleep -Seconds 2
  Start-Process -FilePath $arduinoIde -ArgumentList @("`"$relaySketch`"")
}

Write-Host ''
Write-Host 'PREPARATION COMPLETE' -ForegroundColor Green
Write-Host '1. Upload lte_relay.ino to the WROVER/A7670 FIRST.'
Write-Host '   Board: ESP32 Dev Module; Huge APP; PSRAM Enabled.'
Write-Host '2. Upload telemetry_v2.ino to the ESP32-C3 SECOND.'
Write-Host '   Board: ESP32C3 Dev Module; USB CDC On Boot Enabled.'
Write-Host '3. Keep the computer awake; dashboard and tunnel run hidden.'
Write-Host '4. Open http://127.0.0.1:8000/live to watch motor temperature.'
Write-Host 'The telemetry API key was not printed.'
