# Captures temporally continuous path-traced optical debug-view orbits. Each view runs in a
# fresh process so its RR history is uncontaminated by debug-mode changes. The engine writes
# canonical RGBA8 frames and metadata; this recipe adds PNGs and a suite manifest.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/glass-sphere-test/glass-sphere-test.gameproject',
    [string]$Target = 'glass sphere',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/pt-optical-orbit',
    [ValidateSet('Project', 'Enabled', 'Disabled')]
    [string]$MirrorChainPsr = 'Project',
    [string[]]$Modes = @(
        'raw-radiance', 'final-rr', 'forced-reset-oracle', 'rr-temporal-validity-off',
        'rr-temporal-validity', 'rr-transmission-temporal-validity', 'bloom-off',
        'raw-reflection', 'reconstructed-reflection', 'reflection-delta',
        'reflection-reprojection', 'reflection-replay-status',
        'raw-transmission', 'reconstructed-transmission', 'transmission-delta',
        'transmission-reprojection', 'transmission-replay-status',
        'coverage-fresnel'
    ),
    [ValidateRange(1, 2000)][int]$WarmupFrames = 120,
    [ValidateRange(2, 20000)][int]$OrbitFrames = 180,
    [ValidateRange(1, 16)][int]$Revolutions = 1,
    [ValidateRange(1, 20000)][int]$FrameStride = 12,
    [ValidateRange(30, 1800)][int]$TimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$projectSha256 = (Get-FileHash -LiteralPath $projectPath -Algorithm SHA256).Hash
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
if ($FrameStride -gt $OrbitFrames) { throw 'FrameStride cannot exceed OrbitFrames.' }
$Modes = @($Modes | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } |
    Where-Object { $_ } | Select-Object -Unique)
if ($Modes.Count -eq 0) { throw 'At least one capture mode is required.' }
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$timestamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$outputBase = Join-Path $repoRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $outputBase | Out-Null
$outputRoot = Join-Path $outputBase $timestamp
New-Item -ItemType Directory -Path $outputRoot | Out-Null
$revision = (& git -c "safe.directory=$($repoRoot.Replace('\','/'))" rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve validation revision.' }

$environmentNames = @(
    'GAME_ENGINE_AUTO_OPEN', 'GAME_ENGINE_OPTICAL_CAPTURE_OUTPUT',
    'GAME_ENGINE_OPTICAL_CAPTURE_MODE', 'GAME_ENGINE_OPTICAL_CAPTURE_TARGET',
    'GAME_ENGINE_OPTICAL_CAPTURE_WARMUP_FRAMES', 'GAME_ENGINE_OPTICAL_CAPTURE_ORBIT_FRAMES',
    'GAME_ENGINE_OPTICAL_CAPTURE_REVOLUTIONS', 'GAME_ENGINE_OPTICAL_CAPTURE_FRAME_STRIDE',
    'GAME_ENGINE_CAPTURE_PT_MIRROR_CHAIN_PSR',
    'GAME_ENGINE_FRAME_DEBUG', 'GAME_ENGINE_LOG'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Convert-OrbitFrame([System.IO.FileInfo]$MetadataFile) {
    $metadata = Get-Content -LiteralPath $MetadataFile.FullName -Raw | ConvertFrom-Json
    $rgbaPath = Join-Path $MetadataFile.Directory.FullName ([string]$metadata.rgba)
    $pngPath = [System.IO.Path]::ChangeExtension($MetadataFile.FullName, '.png')
    $width = [int]$metadata.extent[0]
    $height = [int]$metadata.extent[1]
    & $ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba `
        -video_size "${width}x${height}" -i $rgbaPath -frames:v 1 $pngPath
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $pngPath)) {
        throw "Could not convert $rgbaPath to PNG."
    }
    return $pngPath
}

function Invoke-OpticalMode([string]$Mode) {
    $modeDirectory = Join-Path $outputRoot $Mode
    New-Item -ItemType Directory -Path $modeDirectory | Out-Null
    $stdout = Join-Path $modeDirectory 'stdout.log'
    $stderr = Join-Path $modeDirectory 'stderr.log'
    $manifestPath = Join-Path $modeDirectory 'capture.json'

    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_OPTICAL_CAPTURE_OUTPUT = $modeDirectory
    $env:GAME_ENGINE_OPTICAL_CAPTURE_MODE = $Mode
    $env:GAME_ENGINE_OPTICAL_CAPTURE_TARGET = $Target
    $env:GAME_ENGINE_OPTICAL_CAPTURE_WARMUP_FRAMES = [string]$WarmupFrames
    $env:GAME_ENGINE_OPTICAL_CAPTURE_ORBIT_FRAMES = [string]$OrbitFrames
    $env:GAME_ENGINE_OPTICAL_CAPTURE_REVOLUTIONS = [string]$Revolutions
    $env:GAME_ENGINE_OPTICAL_CAPTURE_FRAME_STRIDE = [string]$FrameStride
    if ($MirrorChainPsr -eq 'Project') {
        Remove-Item Env:GAME_ENGINE_CAPTURE_PT_MIRROR_CHAIN_PSR -ErrorAction SilentlyContinue
    } else {
        $env:GAME_ENGINE_CAPTURE_PT_MIRROR_CHAIN_PSR =
            if ($MirrorChainPsr -eq 'Enabled') { '1' } else { '0' }
    }
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'

    Write-Host "Capturing $Mode ..."
    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (!$process.HasExited -and !(Test-Path -LiteralPath $manifestPath) `
        -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
    }
    if (!(Test-Path -LiteralPath $manifestPath)) {
        if (!$process.HasExited) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
        }
        throw "Optical capture $Mode did not complete within ${TimeoutSeconds}s."
    }

    # A completed manifest is the transactional capture boundary. Some driver/Streamline builds
    # can take an unbounded time in process teardown, so give normal shutdown a short grace period
    # and then end only this already-complete child process.
    $exitedCleanly = $process.HasExited -or $process.WaitForExit(10000)
    if (!$exitedCleanly) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
    else {
        # Start-Process can report HasExited before its PowerShell wrapper has populated ExitCode.
        # The parameterless wait returns immediately here and completes that bookkeeping.
        $process.WaitForExit()
        $process.Refresh()
        $exitCode = $process.ExitCode
        if ($null -ne $exitCode -and $exitCode -ne 0) {
            throw "Optical capture $Mode exited with $exitCode."
        }
    }

    $diagnostics = @(Select-String -LiteralPath $stderr -Pattern (
        'streamline.*(error|warn)|D3D12.*(error|warning)|LIVE OBJECT|device removed|DXGI_ERROR|outcome=failed') `
        -CaseSensitive:$false | Where-Object { $_.Line -notmatch 'ModifyExistingFolderDACL' })
    if ($diagnostics.Count -ne 0) {
        throw "Optical capture $Mode contains $($diagnostics.Count) SDK/D3D diagnostic matches."
    }
    $frameMetadata = @(Get-ChildItem -LiteralPath $modeDirectory -Filter 'frame-*.json' | Sort-Object Name)
    if ($frameMetadata.Count -eq 0) { throw "Optical capture $Mode contains no frames." }
    $pngs = @($frameMetadata | ForEach-Object { Convert-OrbitFrame $_ })
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    return [pscustomobject]@{
        mode = $Mode
        manifest = $manifestPath
        capture_count = $frameMetadata.Count
        screenshots = $pngs
        stdout = $stdout
        stderr = $stderr
        exited_cleanly = $exitedCleanly
        teardown_timed_out = !$exitedCleanly
        start_camera = $manifest.start_camera
        target_world = $manifest.target_world
    }
}

try {
    $captures = @($Modes | ForEach-Object { Invoke-OpticalMode $_ })
    $baselinePose = $captures[0].start_camera | ConvertTo-Json -Compress -Depth 5
    $baselineTarget = $captures[0].target_world -join ','
    $mismatches = @($captures | Where-Object {
        ($_.start_camera | ConvertTo-Json -Compress -Depth 5) -ne $baselinePose -or
        ($_.target_world -join ',') -ne $baselineTarget
    })
    if ($mismatches.Count -ne 0) {
        throw "Orbit modes differ in starting pose/target: $(($mismatches.mode) -join ', ')."
    }
    [ordered]@{
        record_type = 'pt_optical_orbit_suite'
        schema_version = 2
        revision = $revision
        project = $projectPath
        project_sha256 = $projectSha256
        target = $Target
        mirror_chain_psr = $MirrorChainPsr
        warmup_frames = $WarmupFrames
        orbit_frames = $OrbitFrames
        revolutions = $Revolutions
        frame_stride = $FrameStride
        captures = $captures
        result = 'PASS'
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (
        Join-Path $outputRoot 'optical-orbit-manifest.json') -Encoding utf8
    Write-Host "PT optical orbit suite PASS: $outputRoot" -ForegroundColor Green
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
