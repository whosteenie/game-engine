# Captures the settled S2-P1 EV matrix from fresh processes. Production rendering is unchanged;
# GAME_ENGINE_S2P1_* selects existing modes and authored exposure only for this opt-in gate.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/cornell-box-test/cornell-box-test.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s2p1/exposure-matrix',
    [ValidateRange(1, 30)][int]$WarmupSeconds = 1,
    [ValidateRange(1, 2000)][int]$WarmupFrames = 120,
    [ValidateRange(1, 300)][int]$SampleFrames = 10,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 180
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$outputRoot = Join-Path $repoRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$outputRoot = (Resolve-Path $outputRoot).Path
$revision = (& git -c "safe.directory=$($repoRoot.Replace('\','/'))" rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve validation revision.' }

$modes = @(
    'direct',
    'dlss-dlaa', 'dlss-quality', 'dlss-balanced', 'dlss-performance', 'dlss-ultra-performance',
    'rr-dlaa', 'rr-quality', 'rr-balanced', 'rr-performance', 'rr-ultra-performance')
$exposures = @(-2, 0, 2)
$environmentNames = @(
    'GAME_ENGINE_AUTO_OPEN', 'GAME_ENGINE_BENCHMARK_OUTPUT',
    'GAME_ENGINE_BENCHMARK_WARMUP_SECONDS', 'GAME_ENGINE_BENCHMARK_WARMUP_FRAMES',
    'GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES', 'GAME_ENGINE_CAPTURE_IMAGE_OUTPUT',
    'GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT', 'GAME_ENGINE_CAPTURE_MANIFEST_INPUT',
    'GAME_ENGINE_CAPTURE_REVISION', 'GAME_ENGINE_CAPTURE_COMPARISON_MODE',
    'GAME_ENGINE_S0P5_CAPTURE', 'GAME_ENGINE_S1P6_CAPTURE_MODE',
    'GAME_ENGINE_S2P1_CAPTURE_MODE', 'GAME_ENGINE_S2P1_EXPOSURE_EV',
    'GAME_ENGINE_FRAME_DEBUG', 'GAME_ENGINE_LOG')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $middle = [Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Convert-RgbaToPng([string]$RgbaPath, [string]$MetadataPath, [string]$PngPath) {
    $metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
    $width = [int]$metadata.extent[0]
    $height = [int]$metadata.extent[1]
    & $ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba `
        -video_size "${width}x${height}" -i $RgbaPath -frames:v 1 $PngPath
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $PngPath)) {
        throw "Could not convert $RgbaPath to PNG."
    }
}

function Invoke-ExposureCapture([string]$Mode, [int]$ExposureEv) {
    $evName = if ($ExposureEv -lt 0) { "evm$(-$ExposureEv)" } else { "evp$ExposureEv" }
    $captureDirectory = Join-Path $outputRoot "$Mode-$evName"
    New-Item -ItemType Directory -Force -Path $captureDirectory | Out-Null
    $stdout = Join-Path $captureDirectory 'stdout.log'
    $stderr = Join-Path $captureDirectory 'stderr.log'
    $timings = Join-Path $captureDirectory 'timings.csv'
    $image = Join-Path $captureDirectory 'output.rgba'
    $manifest = Join-Path $captureDirectory 'manifest.json'
    $png = Join-Path $captureDirectory 'output.png'
    $caseArtifacts = @(
        $stdout, $stderr, $timings, $image, "$image.json", $manifest, $png)
    foreach ($artifact in $caseArtifacts) {
        Remove-Item -LiteralPath $artifact -Force -ErrorAction SilentlyContinue
    }

    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_BENCHMARK_OUTPUT = $timings
    $env:GAME_ENGINE_BENCHMARK_WARMUP_SECONDS = [string]$WarmupSeconds
    $env:GAME_ENGINE_BENCHMARK_WARMUP_FRAMES = [string]$WarmupFrames
    $env:GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES = [string]$SampleFrames
    $env:GAME_ENGINE_CAPTURE_IMAGE_OUTPUT = $image
    $env:GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT = $manifest
    Remove-Item Env:GAME_ENGINE_CAPTURE_MANIFEST_INPUT -ErrorAction SilentlyContinue
    Remove-Item Env:GAME_ENGINE_S1P6_CAPTURE_MODE -ErrorAction SilentlyContinue
    $env:GAME_ENGINE_CAPTURE_REVISION = $revision
    $env:GAME_ENGINE_CAPTURE_COMPARISON_MODE = 'statistical'
    $env:GAME_ENGINE_S0P5_CAPTURE = '1'
    $env:GAME_ENGINE_S2P1_CAPTURE_MODE = $Mode
    $env:GAME_ENGINE_S2P1_EXPOSURE_EV = [string]$ExposureEv
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'

    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $requiredArtifacts = @($timings, $image, "$image.json", $manifest)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline -and !$process.HasExited) {
        if (@($requiredArtifacts | Where-Object { !(Test-Path -LiteralPath $_) }).Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    }
    $missing = @($requiredArtifacts | Where-Object { !(Test-Path -LiteralPath $_) })
    if ($missing.Count -ne 0 -and !$process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
    if ($missing.Count -ne 0) { throw "$Mode EV $ExposureEv is missing: $($missing -join ', ')." }
    $exitedCleanly = $process.HasExited -or $process.WaitForExit(10000)
    if (!$exitedCleanly) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    } else {
        $process.Refresh()
        $exitCode = $process.ExitCode
        if ($null -ne $exitCode -and $exitCode -ne 0) {
            throw "$Mode EV $ExposureEv exited with $exitCode."
        }
    }

    $diagnostics = @(Select-String -LiteralPath $stderr -Pattern (
        'streamline.*(error|warn)|D3D12.*(error|warning)|LIVE OBJECT|device removed|DXGI_ERROR|outcome=failed') `
        -CaseSensitive:$false)
    if ($diagnostics.Count -ne 0) {
        throw "$Mode EV $ExposureEv contains $($diagnostics.Count) SDK/D3D12 diagnostic matches."
    }
    if ($Mode -ne 'direct') {
        $feature = if ($Mode.StartsWith('rr-')) { 'rr' } else { 'dlss' }
        $evaluations = @(Select-String -LiteralPath $stderr -Pattern "feature=$feature .*outcome=evaluated")
        if ($evaluations.Count -lt $WarmupFrames) {
            throw "$Mode EV $ExposureEv settled only $($evaluations.Count) evaluated frames."
        }
        $policy = @(Select-String -LiteralPath $stderr -Pattern 'reconstruction-exposure')
        if ($policy.Count -eq 0) { throw "$Mode EV $ExposureEv is missing exposure policy diagnostics." }
        if ($feature -eq 'rr' -and $policy[-1].Line -notmatch 'preExposure=omitted.*exposureScale=omitted') {
            throw "$Mode EV $ExposureEv sent unsupported RR exposure guidance."
        }
    }

    Convert-RgbaToPng $image "$image.json" $png
    $samples = @(Import-Csv -LiteralPath $timings)
    return [ordered]@{
        mode = $Mode
        exposure_ev = $ExposureEv
        display_scale = [Math]::Pow(2.0, $ExposureEv)
        settled_frames = if ($Mode -eq 'direct') { $WarmupFrames } else { $evaluations.Count }
        median_gpu_ms = Get-Median @($samples | ForEach-Object { [double]$_.frame_gpu_span_ms })
        median_dlss_evaluate_ms = Get-Median @($samples | ForEach-Object { [double]$_.dlss_evaluate_ms })
        median_cpu_frame_ms = Get-Median @($samples | ForEach-Object { [double]$_.cpu_frame_ms })
        screenshot = $png
        timings = $timings
        manifest = $manifest
        stdout = $stdout
        stderr = $stderr
        sdk_d3d12_diagnostic_matches = 0
        exited_cleanly = $exitedCleanly
    }
}

try {
    $captures = @()
    foreach ($mode in $modes) {
        foreach ($ev in $exposures) {
            Write-Host "Capturing $mode at EV $ev..."
            $captures += Invoke-ExposureCapture $mode $ev
        }
    }
    $result = [ordered]@{
        record_type = 's2p1_exposure_matrix'
        schema_version = 1
        revision = $revision
        project = $projectPath
        authored_ev = $exposures
        expected_display_scales = @(0.25, 1.0, 4.0)
        endpoint_ratio = 16.0
        captures = $captures
        sdk_d3d12_diagnostic_matches = 0
        result = 'PASS'
    }
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (
        Join-Path $outputRoot 's2-p1-exposure-manifest.json') -Encoding utf8
    Write-Host "S2-P1 exposure matrix PASS: $outputRoot" -ForegroundColor Green
}
finally {
    foreach ($name in $environmentNames) {
        if ($null -eq $savedEnvironment[$name]) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
        }
    }
}
