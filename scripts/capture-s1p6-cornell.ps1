# Captures the S1-P6 identical-pose Cornell output set from six fresh processes. The RR guide
# bundle is represented by its three authoritative guide targets.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/cornell-box-test/cornell-box-test.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s1p6/cornell',
    [ValidateRange(1, 30)][int]$WarmupSeconds = 2,
    [ValidateRange(1, 2000)][int]$RealtimeWarmupFrames = 120,
    [ValidateRange(1, 20000)][int]$ReferenceWarmupFrames = 512,
    [ValidateRange(1, 300)][int]$SampleFrames = 30
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$outputRoot = Join-Path $repoRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$outputRoot = (Resolve-Path $outputRoot).Path
$revision = (& git -c "safe.directory=$($repoRoot.Replace('\','/'))" rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve validation revision.' }

$environmentNames = @(
    'GAME_ENGINE_AUTO_OPEN', 'GAME_ENGINE_BENCHMARK_OUTPUT',
    'GAME_ENGINE_BENCHMARK_WARMUP_SECONDS', 'GAME_ENGINE_BENCHMARK_WARMUP_FRAMES',
    'GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES', 'GAME_ENGINE_CAPTURE_IMAGE_OUTPUT',
    'GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT', 'GAME_ENGINE_CAPTURE_MANIFEST_INPUT',
    'GAME_ENGINE_CAPTURE_REVISION', 'GAME_ENGINE_CAPTURE_COMPARISON_MODE',
    'GAME_ENGINE_S0P5_CAPTURE', 'GAME_ENGINE_S1P6_CAPTURE_MODE',
    'GAME_ENGINE_FRAME_DEBUG', 'GAME_ENGINE_LOG')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
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

function Invoke-CornellCapture([string]$Mode, [int]$WarmupFrames) {
    $modeDirectory = Join-Path $outputRoot $Mode
    New-Item -ItemType Directory -Force -Path $modeDirectory | Out-Null
    $stdout = Join-Path $modeDirectory 'stdout.log'
    $stderr = Join-Path $modeDirectory 'stderr.log'
    $timings = Join-Path $modeDirectory 'timings.csv'
    $image = Join-Path $modeDirectory 'output.rgba'
    $manifest = Join-Path $modeDirectory 'manifest.json'
    $png = Join-Path $modeDirectory 'output.png'

    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_BENCHMARK_OUTPUT = $timings
    $env:GAME_ENGINE_BENCHMARK_WARMUP_SECONDS = [string]$WarmupSeconds
    $env:GAME_ENGINE_BENCHMARK_WARMUP_FRAMES = [string]$WarmupFrames
    $env:GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES = [string]$SampleFrames
    $env:GAME_ENGINE_CAPTURE_IMAGE_OUTPUT = $image
    $env:GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT = $manifest
    Remove-Item Env:GAME_ENGINE_CAPTURE_MANIFEST_INPUT -ErrorAction SilentlyContinue
    $env:GAME_ENGINE_CAPTURE_REVISION = $revision
    $env:GAME_ENGINE_CAPTURE_COMPARISON_MODE = 'statistical'
    $env:GAME_ENGINE_S0P5_CAPTURE = '1'
    $env:GAME_ENGINE_S1P6_CAPTURE_MODE = $Mode
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'

    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $requiredArtifacts = @($timings, $image, "$image.json", $manifest)
    $deadline = [DateTime]::UtcNow.AddSeconds(240)
    while ([DateTime]::UtcNow -lt $deadline -and !$process.HasExited) {
        $missingArtifacts = @($requiredArtifacts | Where-Object { !(Test-Path -LiteralPath $_) })
        if ($missingArtifacts.Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    }
    $missingArtifacts = @($requiredArtifacts | Where-Object { !(Test-Path -LiteralPath $_) })
    if ($missingArtifacts.Count -ne 0 -and !$process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
    foreach ($required in $requiredArtifacts) {
        if (!(Test-Path -LiteralPath $required)) { throw "Cornell capture $Mode is missing $required." }
    }
    $exitedCleanly = $process.HasExited -or $process.WaitForExit(10000)
    if (!$exitedCleanly) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    } else {
        $process.Refresh()
        $exitCode = $process.ExitCode
        if ($null -ne $exitCode -and $exitCode -ne 0) {
            throw "Cornell capture $Mode exited with $exitCode."
        }
    }
    $diagnostics = @(Select-String -LiteralPath $stderr -Pattern (
        'streamline.*(error|warn)|D3D12.*(error|warning)|LIVE OBJECT|device removed|DXGI_ERROR|outcome=failed') `
        -CaseSensitive:$false)
    if ($diagnostics.Count -ne 0) {
        throw "Cornell capture $Mode contains $($diagnostics.Count) SDK/D3D diagnostic matches."
    }
    Convert-RgbaToPng $image "$image.json" $png
    return [pscustomobject]@{
        mode = $Mode
        warmup_frames = $WarmupFrames
        manifest = $manifest
        timings = $timings
        rgba = $image
        metadata = "$image.json"
        screenshot = $png
        stdout = $stdout
        stderr = $stderr
        exited_cleanly = $exitedCleanly
        teardown_timed_out = !$exitedCleanly
    }
}

try {
    $captures = @(
        Invoke-CornellCapture 'raw-radiance' $RealtimeWarmupFrames
        Invoke-CornellCapture 'rr-diffuse-guide' $RealtimeWarmupFrames
        Invoke-CornellCapture 'rr-specular-guide' $RealtimeWarmupFrames
        Invoke-CornellCapture 'rr-normal-roughness' $RealtimeWarmupFrames
        Invoke-CornellCapture 'final-rr' $RealtimeWarmupFrames
        Invoke-CornellCapture 'reference' $ReferenceWarmupFrames
    )

    $baselineManifest = Get-Content -LiteralPath $captures[0].manifest -Raw | ConvertFrom-Json
    $baselinePose = $baselineManifest.semantic.camera.pose_or_path | ConvertTo-Json -Compress -Depth 5
    $baselineOutputExtent = $baselineManifest.semantic.viewport.output_extent -join 'x'
    $poseFailures = @()
    foreach ($capture in $captures) {
        $manifest = Get-Content -LiteralPath $capture.manifest -Raw | ConvertFrom-Json
        $pose = $manifest.semantic.camera.pose_or_path | ConvertTo-Json -Compress -Depth 5
        $extent = $manifest.semantic.viewport.output_extent -join 'x'
        if ($pose -ne $baselinePose -or $extent -ne $baselineOutputExtent) {
            $poseFailures += $capture.mode
        }
    }
    if ($poseFailures.Count -ne 0) {
        throw "Cornell captures differ in camera pose/output extent: $($poseFailures -join ', ')."
    }

    $result = [ordered]@{
        record_type = 's1p6_cornell_output_set'
        schema_version = 1
        revision = $revision
        project = $projectPath
        identical_pose = $true
        output_extent = $baselineOutputExtent
        camera_pose = $baselineManifest.semantic.camera.pose_or_path
        guide_bundle = @('rr-diffuse-guide', 'rr-specular-guide', 'rr-normal-roughness')
        captures = $captures
        sdk_d3d12_diagnostic_matches = 0
        result = 'PASS'
    }
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (
        Join-Path $outputRoot 'cornell-manifest.json') -Encoding utf8
    Write-Host "S1-P6 Cornell output set PASS: $outputRoot" -ForegroundColor Green
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
