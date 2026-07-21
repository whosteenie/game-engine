# Generates an opaque-only Cornell fixture and captures three fresh-process timing/reuse repeats.
[CmdletBinding()]
param(
    [string]$SourceProject = 'test_proj/cornell-box-test/cornell-box-test.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s1p6/opaque-timing',
    [ValidateRange(2, 5)][int]$Repeats = 3,
    [ValidateRange(1, 30)][int]$WarmupSeconds = 2,
    [ValidateRange(30, 2000)][int]$WarmupFrames = 120,
    [ValidateRange(30, 1000)][int]$SampleFrames = 120
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sourcePath = (Resolve-Path (Join-Path $repoRoot $SourceProject)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
$outputRoot = Join-Path $repoRoot $OutputDirectory
$fixtureRoot = Join-Path $outputRoot 'fixture'
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null
$fixturePath = Join-Path $fixtureRoot 'opaque-cornell.gameproject'
$runtimeFixturePath = Join-Path (Split-Path $sourcePath) 's1p6-opaque.generated.gameproject'
$sourceText = Get-Content -LiteralPath $sourcePath -Raw
$fixtureText = [regex]::Replace(
    $sourceText,
    '"transmission"\s*:\s*(?!0(?:\.0+)?(?:[,\r\n]))[-+0-9.eE]+',
    '"transmission": 0.0')
$fixtureText | Set-Content -LiteralPath $fixturePath -Encoding utf8
$fixtureText | Set-Content -LiteralPath $runtimeFixturePath -Encoding utf8
$fixtureValidation = Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json
$transmissiveCount = @($fixtureValidation.scene.objects | Where-Object {
    $_.PSObject.Properties.Name -contains 'material' -and [double]$_.material.transmission -gt 0.0
}).Count
if ($transmissiveCount -ne 0) { throw "Opaque fixture still has $transmissiveCount transmissive materials." }
$revision = (& git -c "safe.directory=$($repoRoot.Replace('\','/'))" rev-parse HEAD).Trim()

$names = @('GAME_ENGINE_AUTO_OPEN','GAME_ENGINE_BENCHMARK_OUTPUT',
    'GAME_ENGINE_BENCHMARK_WARMUP_SECONDS','GAME_ENGINE_BENCHMARK_WARMUP_FRAMES',
    'GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES','GAME_ENGINE_CAPTURE_REVISION',
    'GAME_ENGINE_S1P6_CAPTURE_MODE','GAME_ENGINE_FRAME_DEBUG','GAME_ENGINE_LOG',
    'GAME_ENGINE_CAPTURE_IMAGE_OUTPUT','GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT',
    'GAME_ENGINE_CAPTURE_MANIFEST_INPUT','GAME_ENGINE_S0P5_CAPTURE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $count = $sorted.Count
    if (($count % 2) -eq 1) { return [double]$sorted[[int]($count / 2)] }
    return ([double]$sorted[$count / 2 - 1] + [double]$sorted[$count / 2]) / 2.0
}
function Get-Percentile([double[]]$Values, [double]$Fraction) {
    $sorted = @($Values | Sort-Object)
    return [double]$sorted[[Math]::Min($sorted.Count - 1, [Math]::Ceiling($Fraction * $sorted.Count) - 1)]
}

try {
    $runs = @()
    for ($index = 1; $index -le $Repeats; ++$index) {
        $runDirectory = Join-Path $outputRoot ("run-{0:D2}" -f $index)
        New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
        $timings = Join-Path $runDirectory 'timings.csv'
        $stdout = Join-Path $runDirectory 'stdout.log'
        $stderr = Join-Path $runDirectory 'stderr.log'
        $env:GAME_ENGINE_AUTO_OPEN = $runtimeFixturePath
        $env:GAME_ENGINE_BENCHMARK_OUTPUT = $timings
        $env:GAME_ENGINE_BENCHMARK_WARMUP_SECONDS = [string]$WarmupSeconds
        $env:GAME_ENGINE_BENCHMARK_WARMUP_FRAMES = [string]$WarmupFrames
        $env:GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES = [string]$SampleFrames
        $env:GAME_ENGINE_CAPTURE_REVISION = $revision
        $env:GAME_ENGINE_S1P6_CAPTURE_MODE = 'final-rr'
        $env:GAME_ENGINE_FRAME_DEBUG = '1'
        $env:GAME_ENGINE_LOG = '1'
        foreach ($unset in @('GAME_ENGINE_CAPTURE_IMAGE_OUTPUT','GAME_ENGINE_CAPTURE_MANIFEST_OUTPUT',
            'GAME_ENGINE_CAPTURE_MANIFEST_INPUT','GAME_ENGINE_S0P5_CAPTURE')) {
            Remove-Item "Env:$unset" -ErrorAction SilentlyContinue
        }
        $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
            -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds(180)
        $rows = @()
        while ([DateTime]::UtcNow -lt $deadline -and !$process.HasExited) {
            if (Test-Path -LiteralPath $timings) {
                $rows = @(Import-Csv -LiteralPath $timings)
                if ($rows.Count -eq $SampleFrames) { break }
            }
            Start-Sleep -Milliseconds 500
        }
        if (Test-Path -LiteralPath $timings) {
            $rows = @(Import-Csv -LiteralPath $timings)
        }
        if ($rows.Count -ne $SampleFrames) {
            if (!$process.HasExited) { Stop-Process -Id $process.Id -Force; $process.WaitForExit() }
            throw "Opaque timing run $index did not produce $SampleFrames samples."
        }
        $exitedCleanly = $process.HasExited -or $process.WaitForExit(10000)
        if (!$exitedCleanly) {
            Stop-Process -Id $process.Id -Force; $process.WaitForExit()
        } else {
            $process.Refresh()
            $exitCode = $process.ExitCode
            if ($null -ne $exitCode -and $exitCode -ne 0) {
                throw "Opaque timing run $index exited with $exitCode."
            }
        }
        $diagnostics = @(Select-String -LiteralPath $stderr -Pattern (
            'streamline.*(error|warn)|D3D12.*(error|warning)|LIVE OBJECT|device removed|DXGI_ERROR|outcome=failed') `
            -CaseSensitive:$false)
        if ($diagnostics.Count -ne 0) { throw "Opaque timing run $index has SDK/D3D diagnostics." }
        $historyLines = @(Select-String -LiteralPath $stderr -Pattern '^\[frame\] history-key ' |
            ForEach-Object Line)
        $schedules = @($historyLines | Where-Object { $_ -match ' event=schedule ' })
        $compatibles = @($historyLines | Where-Object { $_ -match ' event=compatible ' })
        $opticalResets = @($schedules | Where-Object { $_ -match 'reason_bits=0x[0-9A-Fa-f]*4[0-9A-Fa-f]{2}' })
        [double[]]$gpu = @($rows | ForEach-Object { [double]$_.frame_gpu_span_ms })
        [double[]]$cpu = @($rows | ForEach-Object { [double]$_.cpu_frame_ms })
        $gpuMedian = Get-Median $gpu
        [double[]]$gpuDeviation = @($gpu | ForEach-Object { [Math]::Abs($_ - $gpuMedian) })
        $runs += [pscustomobject]@{
            run = $index
            samples = $rows.Count
            gpu_median_ms = $gpuMedian
            gpu_p95_ms = Get-Percentile $gpu 0.95
            gpu_mad_ms = Get-Median $gpuDeviation
            cpu_median_ms = Get-Median $cpu
            history_schedules = $schedules.Count
            history_compatible = $compatibles.Count
            optical_resets = $opticalResets.Count
            timings = $timings
            stdout = $stdout
            stderr = $stderr
            exited_cleanly = $exitedCleanly
            teardown_timed_out = !$exitedCleanly
        }
    }
    [double[]]$medians = @($runs | ForEach-Object { $_.gpu_median_ms })
    $minMedian = ($medians | Measure-Object -Minimum).Minimum
    $maxMedian = ($medians | Measure-Object -Maximum).Maximum
    $relativeRange = if ($minMedian -gt 0) { ($maxMedian - $minMedian) / $minMedian } else { 1.0 }
    $maxMad = ($runs.gpu_mad_ms | Measure-Object -Maximum).Maximum
    $noiseLimit = [Math]::Max(0.05, (3.0 * $maxMad) / [Math]::Max($minMedian, 0.0001))
    $reuseFrames = ($runs.history_compatible | Measure-Object -Sum).Sum
    $resetFrames = ($runs.history_schedules | Measure-Object -Sum).Sum
    $pass = $relativeRange -le $noiseLimit -and ($runs.optical_resets | Measure-Object -Sum).Sum -eq 0 `
        -and $reuseFrames -gt 0
    $result = [ordered]@{
        record_type = 's1p6_opaque_repeatability'
        schema_version = 1
        revision = $revision
        source_project = $sourcePath
        fixture = $fixturePath
        fixture_transmission = 0.0
        repeats = $runs
        gpu_median_relative_range = $relativeRange
        repeatability_noise_limit = $noiseLimit
        steady_compatible_frames = $reuseFrames
        reset_frames = $resetFrames
        optical_domain_resets = ($runs.optical_resets | Measure-Object -Sum).Sum
        sdk_d3d12_diagnostic_matches = 0
        result = if ($pass) { 'PASS' } else { 'FAIL' }
    }
    $result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (
        Join-Path $outputRoot 'opaque-timing-manifest.json') -Encoding utf8
    if (!$pass) { throw 'Opaque timing/reuse repeatability gate failed.' }
    Write-Host "S1-P6 opaque timing/reuse PASS: $outputRoot" -ForegroundColor Green
}
finally {
    foreach ($name in $names) {
        if ($null -eq $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
    if (Test-Path -LiteralPath $runtimeFixturePath) {
        Remove-Item -LiteralPath $runtimeFixturePath -Force
    }
}
