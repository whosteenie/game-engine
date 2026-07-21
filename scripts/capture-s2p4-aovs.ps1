[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s2p4/aovs',
    [ValidateRange(30, 180)][int]$TimeoutSeconds = 90
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build first." }
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot $OutputDirectory) | Out-Null
$outputRoot = (Resolve-Path (Join-Path $repoRoot $OutputDirectory)).Path
$names = @('GAME_ENGINE_AUTO_OPEN','GAME_ENGINE_LOG','GAME_ENGINE_FRAME_DEBUG',
    'GAME_ENGINE_S2P4_CAPTURE_MODE','GAME_ENGINE_S2P4_WINDOWED',
    'GAME_ENGINE_BENCHMARK_OUTPUT','GAME_ENGINE_BENCHMARK_WARMUP_SECONDS',
    'GAME_ENGINE_BENCHMARK_WARMUP_FRAMES','GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES',
    'GAME_ENGINE_CAPTURE_IMAGE_OUTPUT','GAME_ENGINE_CAPTURE_COMPARISON_MODE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$records = @()
try {
    foreach ($mode in @('motion-vectors','primary-depth')) {
        $dir = Join-Path $outputRoot $mode
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $stdout = Join-Path $dir 'stdout.log'
        $stderr = Join-Path $dir 'stderr.log'
        $timings = Join-Path $dir 'timings.csv'
        $rgba = Join-Path $dir 'capture.rgba8'
        $png = Join-Path $dir 'capture.png'
        Remove-Item -LiteralPath $stdout,$stderr,$timings,$rgba,"$rgba.json",$png `
            -Force -ErrorAction SilentlyContinue
        $env:GAME_ENGINE_AUTO_OPEN = $projectPath
        $env:GAME_ENGINE_LOG = '1'
        $env:GAME_ENGINE_FRAME_DEBUG = '1'
        $env:GAME_ENGINE_S2P4_CAPTURE_MODE = $mode
        $env:GAME_ENGINE_S2P4_WINDOWED = '1'
        $env:GAME_ENGINE_BENCHMARK_OUTPUT = $timings
        $env:GAME_ENGINE_BENCHMARK_WARMUP_SECONDS = '1'
        $env:GAME_ENGINE_BENCHMARK_WARMUP_FRAMES = '60'
        $env:GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES = '10'
        $env:GAME_ENGINE_CAPTURE_IMAGE_OUTPUT = $rgba
        $env:GAME_ENGINE_CAPTURE_COMPARISON_MODE = 'statistical'
        $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            if ((Test-Path -LiteralPath $rgba) -and (Test-Path -LiteralPath "$rgba.json") -and
                (Select-String -LiteralPath $stderr -SimpleMatch 'Shutting down NGX' -Quiet)) { break }
            Start-Sleep -Milliseconds 100
        }
        if (!(Test-Path -LiteralPath $rgba) -or !(Test-Path -LiteralPath "$rgba.json")) {
            if ($null -ne (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) {
                Stop-Process -Id $process.Id -Force
            }
            throw "$mode did not produce the GPU presented-image capture."
        }
        $metadata = Get-Content -LiteralPath "$rgba.json" -Raw | ConvertFrom-Json
        $width = [int]$metadata.extent[0]
        $height = [int]$metadata.extent[1]
        & $ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba `
            -video_size "${width}x${height}" -i $rgba -frames:v 1 $png
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $png)) {
            throw "Could not convert $mode capture to PNG."
        }
        $failures = @(Select-String -LiteralPath $stderr -Pattern (
            'S2-P4 .*contract failed|tagging-failed|options-failed|evaluate-failed|' +
            'D3D12 device was removed|DXGI_ERROR_DEVICE_REMOVED|\[error\] \[dlss\]'))
        # Diagnostic AOV presentation intentionally bypasses reconstruction evaluation. Require
        # the active RR allocation plan plus clean runtime logs here; the active-matrix harness
        # separately validates the evaluated RG16F tag, states, extents, and lifetime contract.
        $contract = @(Select-String -LiteralPath $stderr -Pattern (
            'extent-plan .*feature=rr quality=performance.*source=sdk.*rr-no-arbitrary-drs=true.*active-allocation=s2-p4-plan-owned'))
        if ($failures.Count -ne 0 -or $contract.Count -eq 0 -or !(Test-Path -LiteralPath $timings)) {
            throw "$mode AOV contract validation failed."
        }
        $records += [ordered]@{
            mode = $mode
            extent = @($width,$height)
            contract_records = $contract.Count
            runtime_failures = $failures.Count
            png = $png
            rgba = $rgba
            metadata = "$rgba.json"
            timings = $timings
            stderr = $stderr
        }
    }
    [ordered]@{
        record_type = 's2p4_motion_depth_aovs'
        schema_version = 1
        captures = $records
        result = 'PASS'
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
        Join-Path $outputRoot 's2-p4-aov-manifest.json') -Encoding utf8
    Write-Host "S2-P4 motion/depth AOV captures PASS: $outputRoot" -ForegroundColor Green
}
finally {
    foreach ($name in $names) {
        if ($null -eq $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
}
