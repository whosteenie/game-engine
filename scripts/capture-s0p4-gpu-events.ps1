# Runs the S0-P4 GPU-event gate against DEMO-SCENE.
#
# The script uses capture-pt-nsight.ps1 for the Nsight Graphics trace/frame capture and the engine's
# benchmark CSV path for matched marker-off/marker-on timings. It temporarily changes only
# ptConvergenceMode in DEMO-SCENE for the reference-accumulation capture, restoring the file in
# finally even if a capture fails.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$BuildDir = 'build',
    [string]$NsightRoot = '',
    [ValidateRange(1, 120)][int]$WarmupSeconds = 10,
    [ValidateRange(1, 2000)][int]$WarmupFrames = 120,
    [ValidateRange(30, 5000)][int]$SampleFrames = 300,
    [ValidateRange(1, 15)][int]$TraceFrames = 5,
    [string]$OutputDirectory = '',
    [switch]$KeepProjectBackup
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
$captureScript = Join-Path $scriptRoot 'capture-pt-nsight.ps1'
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
if (!(Test-Path -LiteralPath $captureScript)) { throw "Missing shared Nsight capture script: $captureScript" }

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot ('artifacts\s0p4-gpu-events\demo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path

$markerNames = @(
    'PT.Megakernel',
    'PT.ReSTIR.Temporal',
    'PT.ReSTIR.Spatial',
    'PT.ReSTIR.GiBoilingFilter',
    'PT.SurfaceHistory',
    'PT.RR.Preparation',
    'PT.ReferenceAccumulation'
)

function Write-CaptureConfig {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$SessionName, [Parameter(Mandatory)][string]$ProjectPath)
    [ordered]@{
        sessionName = $SessionName
        engineExe = "$BuildDir/$Config/game-engine.exe"
        workingDirectory = "$BuildDir/$Config"
        warmupSeconds = $WarmupSeconds
        traceFrames = $TraceFrames
        captureTimestampBaseline = $true
        baselineWarmupSeconds = $WarmupSeconds
        baselineWarmupFrames = $WarmupFrames
        baselineSampleFrames = $SampleFrames
        # GPU Trace supplies the required event tree. Do not invoke ngfx-capture.exe: its DLSS/NVNGX
        # interception path crashes on this application and is not required for the S0-P4 gate.
        captureFrame = $false
        views = @([ordered]@{ name = 'demo'; project = $ProjectPath })
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Invoke-S0P4Capture {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$ProjectPath,
        [Parameter(Mandatory)][bool]$EventsEnabled,
        [Parameter(Mandatory)][bool]$CaptureGraphics
    )
    $configPath = Join-Path $OutputDirectory "$Name.config.json"
    Write-CaptureConfig -Path $configPath -SessionName $Name -ProjectPath $ProjectPath
    $env:GAME_ENGINE_PT_GPU_EVENTS = if ($EventsEnabled) { '1' } else { '0' }
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $captureScript, '-Config', $configPath, '-OutputDir', $OutputDirectory, '-SessionName', $Name)
    if ($NsightRoot) { $args += @('-NsightRoot', $NsightRoot) }
    if (!$CaptureGraphics) { $args += '-TimestampOnly' }
    & powershell.exe @args
    if ($LASTEXITCODE -ne 0) { throw "S0-P4 capture '$Name' failed with exit code $LASTEXITCODE." }
    return (Join-Path $OutputDirectory $Name)
}

function Read-TimingSummary {
    param([Parameter(Mandatory)][string]$SessionDirectory)
    $path = Join-Path $SessionDirectory 'demo\timestamp-summary.json'
    if (!(Test-Path -LiteralPath $path)) { throw "Missing timestamp summary: $path" }
    return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
}

function Compare-Timings {
    param([Parameter(Mandatory)]$Disabled, [Parameter(Mandatory)]$Enabled)
    $metrics = [ordered]@{}
    foreach ($property in $Disabled.metrics.PSObject.Properties) {
        $enabledMetric = $Enabled.metrics.PSObject.Properties[$property.Name]
        if ($null -eq $enabledMetric -or $null -eq $property.Value -or $null -eq $enabledMetric.Value) { continue }
        $offMs = [double]$property.Value.medianMs
        $onMs = [double]$enabledMetric.Value.medianMs
        $metrics[$property.Name] = [ordered]@{
            markerOffMedianMs = $offMs
            markerOnMedianMs = $onMs
            overheadMs = [Math]::Round($onMs - $offMs, 6)
            overheadPercent = if ($offMs -gt 0.0) { [Math]::Round(100.0 * ($onMs - $offMs) / $offMs, 4) } else { $null }
        }
    }
    if ($metrics.Count -eq 0) { throw 'No common timestamp metrics were available for the marker-overhead comparison.' }
    return $metrics
}

function Assert-CaptureMarkers {
    param([Parameter(Mandatory)][string]$SessionDirectory, [Parameter(Mandatory)][string[]]$RequiredMarkers)
    $textFiles = @(Get-ChildItem -LiteralPath $SessionDirectory -File -Recurse |
        Where-Object { $_.Extension -in @('.csv', '.json', '.log', '.txt') })
    $missing = @()
    foreach ($marker in $RequiredMarkers) {
        $found = $false
        foreach ($file in $textFiles) {
            if (Select-String -LiteralPath $file.FullName -SimpleMatch -Quiet -Pattern $marker) { $found = $true; break }
        }
        if (!$found) { $missing += $marker }
    }
    if ($missing.Count -ne 0) {
        throw "Nsight auto-export did not expose required markers: $($missing -join ', '). Open the saved capture and export the event tree, then rerun with that export in the session directory."
    }
}

$savedMarkerEnvironment = [Environment]::GetEnvironmentVariable('GAME_ENGINE_PT_GPU_EVENTS', 'Process')
$projectBackup = Join-Path $OutputDirectory 'DEMO-SCENE.gameproject.before-s0p4'
$projectModified = $false
try {
    # DEMO-SCENE normally disables GI spatial reuse. S0-P4 needs one frame with both the boiling
    # filter and spatial dispatch active, so change only that test setting and restore it in finally.
    Copy-Item -LiteralPath $projectPath -Destination $projectBackup -Force
    $projectText = Get-Content -LiteralPath $projectPath -Raw
    $spatialReplacementCount = [regex]::Matches($projectText, '"restirGiSpatialEnabled"\s*:\s*false').Count
    if ($spatialReplacementCount -ne 1) { throw "Expected exactly one disabled GI spatial setting in $projectPath; found $spatialReplacementCount." }
    $projectText = [regex]::Replace($projectText, '"restirGiSpatialEnabled"\s*:\s*false', '"restirGiSpatialEnabled": true', 1)
    Set-Content -LiteralPath $projectPath -Value $projectText -Encoding utf8
    $projectModified = $true

    # Matched real-time runs: timing comparison plus the capture containing all real-time scopes.
    $realTimeOff = Invoke-S0P4Capture -Name 'realtime-markers-off' -ProjectPath $projectPath -EventsEnabled $false -CaptureGraphics $false
    $realTimeOn = Invoke-S0P4Capture -Name 'realtime-markers-on' -ProjectPath $projectPath -EventsEnabled $true -CaptureGraphics $true

    # Reference accumulation is mutually exclusive with ReSTIR/RR. Preserve the scene and switch only
    # the project setting for its separate capture, then restore it before returning.
    $projectText = Get-Content -LiteralPath $projectPath -Raw
    $replacementCount = [regex]::Matches($projectText, '"ptConvergenceMode"\s*:\s*"realTime"').Count
    if ($replacementCount -ne 1) { throw "Expected exactly one real-time convergence setting in $projectPath; found $replacementCount." }
    $referenceText = [regex]::Replace($projectText, '"ptConvergenceMode"\s*:\s*"realTime"', '"ptConvergenceMode": "reference"', 1)
    Set-Content -LiteralPath $projectPath -Value $referenceText -Encoding utf8
    $referenceOn = Invoke-S0P4Capture -Name 'reference-markers-on' -ProjectPath $projectPath -EventsEnabled $true -CaptureGraphics $true
    Copy-Item -LiteralPath $projectBackup -Destination $projectPath -Force
    $projectModified = $false

    $overhead = Compare-Timings -Disabled (Read-TimingSummary $realTimeOff) -Enabled (Read-TimingSummary $realTimeOn)
    Assert-CaptureMarkers -SessionDirectory $realTimeOn -RequiredMarkers $markerNames[0..5]
    Assert-CaptureMarkers -SessionDirectory $referenceOn -RequiredMarkers @('PT.Megakernel', 'PT.ReferenceAccumulation')
    [ordered]@{
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
        project = $projectPath
        engine = $enginePath
        markerEnvironment = 'GAME_ENGINE_PT_GPU_EVENTS (0=disabled, 1=enabled)'
        realtimeMarkersOff = $realTimeOff
        realtimeMarkersOn = $realTimeOn
        referenceMarkersOn = $referenceOn
        requiredMarkers = $markerNames
        markerOverhead = $overhead
        gate = 'Pass only after the saved real-time and reference Nsight captures show the listed balanced event scopes and this report contains matched timing deltas.'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 's0p4-result.json') -Encoding utf8
    Write-Host "S0-P4 capture gate artifacts: $OutputDirectory" -ForegroundColor Green
}
finally {
    if ($projectModified -and (Test-Path -LiteralPath $projectBackup)) {
        Copy-Item -LiteralPath $projectBackup -Destination $projectPath -Force
    }
    if (!$KeepProjectBackup -and (Test-Path -LiteralPath $projectBackup)) {
        Remove-Item -LiteralPath $projectBackup -Force
    }
    if ($null -eq $savedMarkerEnvironment) {
        Remove-Item Env:GAME_ENGINE_PT_GPU_EVENTS -ErrorAction SilentlyContinue
    }
    else {
        [Environment]::SetEnvironmentVariable('GAME_ENGINE_PT_GPU_EVENTS', $savedMarkerEnvironment, 'Process')
    }
}
