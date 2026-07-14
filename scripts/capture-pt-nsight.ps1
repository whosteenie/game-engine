# Automated Nsight Graphics GPU Trace / frame capture for the PT fixed-view benchmark.
#
# The target application already honors GAME_ENGINE_AUTO_OPEN, so each project is loaded without
# interacting with the editor. Camera transforms live in the project editor state: make the two
# projects from the same scene and save each fixed camera before running this script.
#
# See devdoc/dxr/pt/performance-roadmap.md §3.2.1 for the configuration and interpretation contract.

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Config,
    [string]$NsightRoot = "",
    [string]$OutputDir = "artifacts/pt-nsight",
    [switch]$SkipFrameCapture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDirectory "..")).Path

function Resolve-ConfigPath {
    param([Parameter(Mandatory)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

function Find-NsightExecutable {
    param(
        [Parameter(Mandatory)][string]$FileName,
        [string]$InstallRoot
    )

    $roots = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($InstallRoot)) {
        [void]$roots.Add((Resolve-ConfigPath $InstallRoot))
    }

    $nvidiaRoot = Join-Path $env:ProgramFiles "NVIDIA Corporation"
    if (Test-Path -LiteralPath $nvidiaRoot) {
        Get-ChildItem -LiteralPath $nvidiaRoot -Directory -Filter "Nsight Graphics*" |
            Sort-Object Name -Descending |
            ForEach-Object { [void]$roots.Add($_.FullName) }
    }

    foreach ($root in $roots) {
        $standardPath = Join-Path $root (Join-Path "host\windows-desktop-nomad-x64" $FileName)
        if (Test-Path -LiteralPath $standardPath) {
            return (Resolve-Path -LiteralPath $standardPath).Path
        }

        $found = Get-ChildItem -LiteralPath $root -Filter $FileName -File -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $found) {
            return $found.FullName
        }
    }

    throw "Could not find $FileName. Install Nsight Graphics, or pass -NsightRoot with its install directory."
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][string]$LogPath
    )

    Write-Host ("> {0} {1}" -f $Executable, ($Arguments -join " ")) -ForegroundColor DarkGray
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Executable @Arguments 2>&1 | Tee-Object -LiteralPath $LogPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }

    if ($exitCode -ne 0) {
        throw "Capture command failed with exit code $exitCode. See $LogPath"
    }
}

function Export-CsvCopiesAsJson {
    param([Parameter(Mandatory)][string]$CaptureDirectory)

    $exports = @()
    $csvFiles = Get-ChildItem -LiteralPath $CaptureDirectory -Filter "*.csv" -File -Recurse |
        Sort-Object FullName
    foreach ($csv in $csvFiles) {
        $rows = @(Import-Csv -LiteralPath $csv.FullName)
        $columns = @()
        if ($rows.Count -gt 0) {
            $columns = @($rows[0].PSObject.Properties.Name)
        }
        $exports += [pscustomobject]@{
            source = $csv.FullName.Substring($CaptureDirectory.Length).TrimStart('\\', '/')
            columns = $columns
            rows = $rows
        }
    }

    $jsonPath = Join-Path $CaptureDirectory "nsight-export.json"
    [pscustomobject]@{ exports = $exports } |
        ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $jsonPath -Encoding utf8
    return $jsonPath
}

function Get-TimestampSummary {
    param([Parameter(Mandatory)][string]$CsvPath)

    $rows = @(Import-Csv -LiteralPath $CsvPath)
    if ($rows.Count -eq 0) { throw "Timestamp capture is empty: $CsvPath" }
    $summary = [ordered]@{ sampleCount = $rows.Count; metrics = [ordered]@{} }
    foreach ($property in $rows[0].PSObject.Properties.Name) {
        if ($property -eq "sample_index") { continue }
        $values = @($rows | ForEach-Object { [double]$_.$property } | Where-Object { $_ -ge 0.0 } | Sort-Object)
        if ($values.Count -eq 0) {
            $summary.metrics[$property] = $null
            continue
        }
        $median = if (($values.Count % 2) -eq 1) {
            $values[[int]($values.Count / 2)]
        } else {
            ($values[$values.Count / 2 - 1] + $values[$values.Count / 2]) / 2.0
        }
        $p95Index = [Math]::Min($values.Count - 1, [Math]::Ceiling($values.Count * 0.95) - 1)
        $summary.metrics[$property] = [ordered]@{
            samples = $values.Count
            medianMs = [Math]::Round($median, 6)
            p95Ms = [Math]::Round($values[$p95Index], 6)
        }
    }
    return [pscustomobject]$summary
}

function Invoke-TimestampBaseline {
    param(
        [Parameter(Mandatory)][string]$EngineExecutable,
        [Parameter(Mandatory)][string]$EngineWorkingDirectory,
        [Parameter(Mandatory)][string]$ProjectPath,
        [Parameter(Mandatory)][string]$CaptureDirectory,
        [Parameter(Mandatory)][int]$WarmupSeconds,
        [Parameter(Mandatory)][int]$WarmupFrames,
        [Parameter(Mandatory)][int]$SampleFrames
    )

    $csvPath = Join-Path $CaptureDirectory "timestamp-samples.csv"
    $logPath = Join-Path $CaptureDirectory "timestamp-capture.log"
    $environmentNames = @(
        "GAME_ENGINE_AUTO_OPEN",
        "GAME_ENGINE_AUTO_OPEN_DEFERRED",
        "GAME_ENGINE_BENCHMARK_OUTPUT",
        "GAME_ENGINE_BENCHMARK_WARMUP_SECONDS",
        "GAME_ENGINE_BENCHMARK_WARMUP_FRAMES",
        "GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES"
    )
    $previousEnvironment = @{}
    foreach ($name in $environmentNames) { $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process") }

    try {
        [Environment]::SetEnvironmentVariable("GAME_ENGINE_AUTO_OPEN", $ProjectPath, "Process")
        [Environment]::SetEnvironmentVariable("GAME_ENGINE_AUTO_OPEN_DEFERRED", "1", "Process")
        [Environment]::SetEnvironmentVariable("GAME_ENGINE_BENCHMARK_OUTPUT", $csvPath, "Process")
        [Environment]::SetEnvironmentVariable("GAME_ENGINE_BENCHMARK_WARMUP_SECONDS", "$WarmupSeconds", "Process")
        [Environment]::SetEnvironmentVariable("GAME_ENGINE_BENCHMARK_WARMUP_FRAMES", "$WarmupFrames", "Process")
        [Environment]::SetEnvironmentVariable("GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES", "$SampleFrames", "Process")
        Push-Location $EngineWorkingDirectory
        try {
            Invoke-NativeCapture -Executable $EngineExecutable -Arguments @() -LogPath $logPath
        }
        finally {
            Pop-Location
        }
    }
    finally {
        foreach ($name in $environmentNames) {
            [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], "Process")
        }
    }

    if (-not (Test-Path -LiteralPath $csvPath)) {
        throw "Engine exited without writing the timestamp baseline: $csvPath"
    }
    $summaryPath = Join-Path $CaptureDirectory "timestamp-summary.json"
    Get-TimestampSummary -CsvPath $csvPath | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $summaryPath -Encoding utf8
    return [pscustomobject]@{ csv = $csvPath; summary = $summaryPath; log = $logPath }
}

$configPath = Resolve-ConfigPath $Config
if (-not (Test-Path -LiteralPath $configPath)) {
    throw "Config file not found: $configPath"
}
$settings = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json

function Get-OptionalConfigValue {
    param(
        [object]$Object,
        [string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) {
        return $property.Value
    }
    return $null
}

foreach ($requiredProperty in @("engineExe", "workingDirectory", "views")) {
    if ($null -eq $settings.PSObject.Properties[$requiredProperty]) {
        throw "Config must specify '$requiredProperty'."
    }
}
if ($settings.views.Count -eq 0) {
    throw "Config must contain at least one view."
}

$engineExe = Resolve-ConfigPath ([string]$settings.engineExe)
$workingDirectory = Resolve-ConfigPath ([string]$settings.workingDirectory)
if (-not (Test-Path -LiteralPath $engineExe)) { throw "Engine executable not found: $engineExe" }
if (-not (Test-Path -LiteralPath $workingDirectory)) { throw "Working directory not found: $workingDirectory" }

$warmupSecondsValue = Get-OptionalConfigValue $settings "warmupSeconds"
$traceFramesValue = Get-OptionalConfigValue $settings "traceFrames"
$warmupSeconds = if ($null -ne $warmupSecondsValue) { [int]$warmupSecondsValue } else { 30 }
$traceFrames = if ($null -ne $traceFramesValue) { [int]$traceFramesValue } else { 5 }
if ($warmupSeconds -lt 1) { throw "warmupSeconds must be at least 1." }
if ($traceFrames -lt 1 -or $traceFrames -gt 15) {
    throw "traceFrames must be in Nsight GPU Trace's supported 1..15 range."
}

$captureFrame = -not $SkipFrameCapture
$captureFrameValue = Get-OptionalConfigValue $settings "captureFrame"
if ($null -ne $captureFrameValue) { $captureFrame = [bool]$captureFrameValue -and -not $SkipFrameCapture }
$captureTimestampBaseline = $true
$captureTimestampBaselineValue = Get-OptionalConfigValue $settings "captureTimestampBaseline"
if ($null -ne $captureTimestampBaselineValue) { $captureTimestampBaseline = [bool]$captureTimestampBaselineValue }
$baselineWarmupSecondsValue = Get-OptionalConfigValue $settings "baselineWarmupSeconds"
$baselineWarmupFramesValue = Get-OptionalConfigValue $settings "baselineWarmupFrames"
$baselineSampleFramesValue = Get-OptionalConfigValue $settings "baselineSampleFrames"
$baselineWarmupSeconds = if ($null -ne $baselineWarmupSecondsValue) { [int]$baselineWarmupSecondsValue } else { 10 }
$baselineWarmupFrames = if ($null -ne $baselineWarmupFramesValue) { [int]$baselineWarmupFramesValue } else { 120 }
$baselineSampleFrames = if ($null -ne $baselineSampleFramesValue) { [int]$baselineSampleFramesValue } else { 300 }
if ($baselineWarmupSeconds -lt 0 -or $baselineWarmupFrames -lt 1 -or $baselineSampleFrames -lt 1) {
    throw "baselineWarmupSeconds must be non-negative; baselineWarmupFrames and baselineSampleFrames must be positive."
}
$architectureValue = Get-OptionalConfigValue $settings "architecture"
$metricSetIdValue = Get-OptionalConfigValue $settings "metricSetId"
$perArchConfigValue = Get-OptionalConfigValue $settings "perArchConfig"
$architecture = if ($null -ne $architectureValue) { [string]$architectureValue } else { "" }
$metricSetId = if ($null -ne $metricSetIdValue) { [string]$metricSetIdValue } else { "" }
$perArchConfig = if ($null -ne $perArchConfigValue) { Resolve-ConfigPath ([string]$perArchConfigValue) } else { "" }
if ($perArchConfig -and -not (Test-Path -LiteralPath $perArchConfig)) {
    throw "perArchConfig not found: $perArchConfig"
}

$ngfx = Find-NsightExecutable -FileName "ngfx.exe" -InstallRoot $NsightRoot
$ngfxCapture = if ($captureFrame) {
    Find-NsightExecutable -FileName "ngfx-capture.exe" -InstallRoot $NsightRoot
}

$sessionNameValue = Get-OptionalConfigValue $settings "sessionName"
$sessionName = if ($null -ne $sessionNameValue -and -not [string]::IsNullOrWhiteSpace([string]$sessionNameValue)) {
    [string]$sessionNameValue
} else {
    "pt-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
}
$sessionDirectory = Join-Path (Resolve-ConfigPath $OutputDir) $sessionName
New-Item -ItemType Directory -Path $sessionDirectory -Force | Out-Null

$manifestViews = @()
foreach ($view in $settings.views) {
    if ($null -eq $view.name -or $null -eq $view.project) {
        throw "Each view must specify both 'name' and 'project'."
    }
    $viewName = [string]$view.name
    if ($viewName.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
        throw "View name contains invalid filename characters: $viewName"
    }
    $project = Resolve-ConfigPath ([string]$view.project)
    if (-not (Test-Path -LiteralPath $project)) { throw "Project not found for '$viewName': $project" }

    $viewDirectory = Join-Path $sessionDirectory $viewName
    New-Item -ItemType Directory -Path $viewDirectory -Force | Out-Null
    $targetEnv = "GAME_ENGINE_AUTO_OPEN=$project;GAME_ENGINE_AUTO_OPEN_DEFERRED=1"

    $timestampBaseline = $null
    if ($captureTimestampBaseline) {
        Write-Host ("Capturing unsmoothed timestamp baseline: {0}" -f $viewName) -ForegroundColor Cyan
        $timestampBaseline = Invoke-TimestampBaseline `
            -EngineExecutable $engineExe `
            -EngineWorkingDirectory $workingDirectory `
            -ProjectPath $project `
            -CaptureDirectory $viewDirectory `
            -WarmupSeconds $baselineWarmupSeconds `
            -WarmupFrames $baselineWarmupFrames `
            -SampleFrames $baselineSampleFrames
    }

    Write-Host ("Capturing GPU Trace: {0}" -f $viewName) -ForegroundColor Cyan
    $gpuTraceArgs = @(
        '--activity=GPU Trace Profiler',
        '--platform=Windows (x86_64)',
        "--exe=$engineExe",
        "--dir=$workingDirectory",
        "--env=$targetEnv",
        "--output-dir=$viewDirectory",
        "--start-after-ms=$($warmupSeconds * 1000)",
        "--limit-to-frames=$traceFrames",
        '--auto-export'
    )
    if ($architecture) { $gpuTraceArgs += "--architecture=$architecture" }
    if ($metricSetId) { $gpuTraceArgs += "--metric-set-id=$metricSetId" }
    if ($perArchConfig) { $gpuTraceArgs += "--per-arch-config-path=$perArchConfig" }
    $gpuTraceLog = Join-Path $viewDirectory "gpu-trace.log"
    Invoke-NativeCapture -Executable $ngfx -Arguments $gpuTraceArgs -LogPath $gpuTraceLog

    $frameCaptureLog = ""
    if ($captureFrame) {
        Write-Host ("Capturing replayable frame: {0}" -f $viewName) -ForegroundColor Cyan
        $frameCaptureArgs = @(
            "--exe=$engineExe",
            "--working-dir=$workingDirectory",
            "--env=$targetEnv",
            "--output-dir=$viewDirectory",
            "--output-file=$viewName",
            "--capture-countdown-timer=$($warmupSeconds * 1000)",
            '--frame-count=1',
            '--terminate-after-capture',
            '--no-hud'
        )
        $frameCaptureLog = Join-Path $viewDirectory "frame-capture.log"
        Invoke-NativeCapture -Executable $ngfxCapture -Arguments $frameCaptureArgs -LogPath $frameCaptureLog
    }

    $jsonExport = Export-CsvCopiesAsJson -CaptureDirectory $viewDirectory
    $files = @(Get-ChildItem -LiteralPath $viewDirectory -File -Recurse |
        Sort-Object FullName |
        ForEach-Object { $_.FullName.Substring($viewDirectory.Length).TrimStart('\\', '/') })
    $manifestViews += [pscustomobject]@{
        name = $viewName
        project = $project
        timestampBaseline = if ($null -ne $timestampBaseline) {
            [pscustomobject]@{
                csv = $timestampBaseline.csv.Substring($sessionDirectory.Length).TrimStart('\\', '/')
                summary = $timestampBaseline.summary.Substring($sessionDirectory.Length).TrimStart('\\', '/')
                log = $timestampBaseline.log.Substring($sessionDirectory.Length).TrimStart('\\', '/')
            }
        } else { $null }
        gpuTraceLog = $gpuTraceLog.Substring($sessionDirectory.Length).TrimStart('\\', '/')
        frameCaptureLog = if ($frameCaptureLog) { $frameCaptureLog.Substring($sessionDirectory.Length).TrimStart('\\', '/') } else { $null }
        csvJson = $jsonExport.Substring($sessionDirectory.Length).TrimStart('\\', '/')
        files = $files
    }
}

$manifestPath = Join-Path $sessionDirectory "manifest.json"
[pscustomobject]@{
    sessionName = $sessionName
    capturedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
    config = $configPath
    engineExe = $engineExe
    workingDirectory = $workingDirectory
    nsight = [pscustomobject]@{
        ngfx = $ngfx
        ngfxCapture = $ngfxCapture
        architecture = $architecture
        metricSetId = $metricSetId
        perArchConfig = $perArchConfig
    }
    warmupSeconds = $warmupSeconds
    traceFrames = $traceFrames
    timestampBaseline = [pscustomobject]@{
        enabled = $captureTimestampBaseline
        warmupSeconds = $baselineWarmupSeconds
        warmupFrames = $baselineWarmupFrames
        sampleFrames = $baselineSampleFrames
    }
    frameCapture = $captureFrame
    views = $manifestViews
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8

Write-Host "Nsight session complete: $sessionDirectory" -ForegroundColor Green
Write-Host "Manifest: $manifestPath" -ForegroundColor Green
