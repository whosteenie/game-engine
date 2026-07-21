# Captures and validates the S2-P5 reconstruction matrix on the real D3D12/Streamline path.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s2p5/mode-matrix',
    [ValidateRange(60, 600)][int]$TimeoutSeconds = 300,
    [switch]$ParseExisting
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot $OutputDirectory) | Out-Null
$outputRoot = (Resolve-Path (Join-Path $repoRoot $OutputDirectory)).Path
$screenshotRoot = Join-Path $outputRoot 'screenshots'
New-Item -ItemType Directory -Force -Path $screenshotRoot | Out-Null
$stdout = Join-Path $outputRoot 'stdout.log'
$stderr = Join-Path $outputRoot 'stderr.log'
$timings = Join-Path $outputRoot 'timings.csv'
$manifest = Join-Path $outputRoot 's2-p5-mode-matrix.json'
if (!$ParseExisting) {
    Remove-Item -LiteralPath $stdout, $stderr, $timings, $manifest -Force -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $screenshotRoot -Filter '*.png' -File -ErrorAction SilentlyContinue |
        Remove-Item -Force
}

$revision = (& git -c "safe.directory=$($repoRoot.Replace('\','/'))" rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve validation revision.' }

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class S2P5WindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
'@

function Save-WindowCapture([System.Diagnostics.Process]$Process, [string]$Path) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $false }
    $rect = New-Object S2P5WindowCapture+Rect
    if (![S2P5WindowCapture]::GetWindowRect($handle, [ref]$rect)) { return $false }
    $width = $rect.R - $rect.L
    $height = $rect.B - $rect.T
    if ($width -le 0 -or $height -le 0) { return $false }
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try { $ok = [S2P5WindowCapture]::PrintWindow($handle, $hdc, 2) }
        finally { $graphics.ReleaseHdc($hdc) }
        if ($ok) { $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png) }
        return $ok
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Get-Percentile([double[]]$Values, [double]$Fraction) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $index = [Math]::Min($sorted.Count - 1, [Math]::Floor(($sorted.Count - 1) * $Fraction))
    return [double]$sorted[$index]
}

function Get-TimingSummary([object[]]$Samples, [string]$Property) {
    $values = @($Samples | ForEach-Object {
        $value = $_.$Property
        if ($null -ne $value -and ![string]::IsNullOrWhiteSpace([string]$value) -and ([double]$value -ge 0.0)) {
            [double]$value
        }
    })
    return [ordered]@{
        samples = $values.Count
        min_ms = if ($values.Count -ne 0) { ($values | Measure-Object -Minimum).Minimum } else { $null }
        median_ms = Get-Percentile $values 0.5
        p95_ms = Get-Percentile $values 0.95
        max_ms = if ($values.Count -ne 0) { ($values | Measure-Object -Maximum).Maximum } else { $null }
    }
}

$names = @(
    'GAME_ENGINE_AUTO_OPEN', 'GAME_ENGINE_FRAME_DEBUG', 'GAME_ENGINE_LOG',
    'GAME_ENGINE_S2P5_MODE_MATRIX', 'GAME_ENGINE_AUTOMATION_DUAL_VIEW',
    'GAME_ENGINE_S1P4_DUAL_OWNERSHIP', 'GAME_ENGINE_S1P4_TRANSITIONS',
    'GAME_ENGINE_S2P4_TRANSITIONS', 'GAME_ENGINE_S2P4_FALLBACK_SMOKE',
    'GAME_ENGINE_BENCHMARK_OUTPUT', 'GAME_ENGINE_BENCHMARK_WARMUP_SECONDS',
    'GAME_ENGINE_BENCHMARK_WARMUP_FRAMES', 'GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$process = $null

try {
    $capturedCells = @{}
    if (!$ParseExisting) {
        $env:GAME_ENGINE_AUTO_OPEN = $projectPath
        $env:GAME_ENGINE_FRAME_DEBUG = '1'
        $env:GAME_ENGINE_LOG = '1'
        $env:GAME_ENGINE_S2P5_MODE_MATRIX = '1'
        $env:GAME_ENGINE_AUTOMATION_DUAL_VIEW = '1'
        foreach ($unset in @(
            'GAME_ENGINE_S1P4_DUAL_OWNERSHIP', 'GAME_ENGINE_S1P4_TRANSITIONS',
            'GAME_ENGINE_S2P4_TRANSITIONS', 'GAME_ENGINE_S2P4_FALLBACK_SMOKE')) {
            Remove-Item "Env:$unset" -ErrorAction SilentlyContinue
        }
        $env:GAME_ENGINE_BENCHMARK_OUTPUT = $timings
        $env:GAME_ENGINE_BENCHMARK_WARMUP_SECONDS = '0'
        $env:GAME_ENGINE_BENCHMARK_WARMUP_FRAMES = '1'
        $env:GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES = '2800'

        $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            if (Test-Path -LiteralPath $stderr) {
                $markers = @(Select-String -LiteralPath $stderr -Pattern 'S2-P5 cell selected index=' |
                    ForEach-Object Line)
                foreach ($line in $markers) {
                    if ($line -notmatch 'index=(\d+) extent-case=(\d+) mode=([^ ]+) ev=(-?\d+(?:\.\d+)?)') {
                        continue
                    }
                    $cellIndex = [int]$Matches[1]
                    $extentCase = [int]$Matches[2]
                    $mode = $Matches[3]
                    $ev = [double]$Matches[4]
                    if ($ev -eq 0.0 -and !$capturedCells.ContainsKey($cellIndex)) {
                        Start-Sleep -Milliseconds 75
                        $path = Join-Path $screenshotRoot (
                            'extent-{0:D1}-cell-{1:D2}-{2}-ev0.png' -f $extentCase, $cellIndex, $mode)
                        if (Save-WindowCapture $process $path) { $capturedCells[$cellIndex] = $path }
                    }
                }
            }
            if ($process.HasExited) { break }
            Start-Sleep -Milliseconds 50
        }
        if (!$process.HasExited) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
            throw "S2-P5 mode matrix timed out after $TimeoutSeconds seconds."
        }
        $process.Refresh()
        $exitCode = $process.ExitCode
        if ($null -ne $exitCode -and $exitCode -ne 0) {
            throw "S2-P5 engine process exited with $exitCode."
        }
    } else {
        foreach ($file in @(Get-ChildItem -LiteralPath $screenshotRoot -Filter '*.png' -File)) {
            if ($file.Name -match 'cell-(\d+)') { $capturedCells[[int]$Matches[1]] = $file.FullName }
        }
    }
    if (!(Test-Path -LiteralPath $timings)) { throw 'S2-P5 timing CSV was not produced.' }

    $allLines = @(Get-Content -LiteralPath $stderr)
    $markerIndices = @()
    for ($i = 0; $i -lt $allLines.Count; ++$i) {
        if ($allLines[$i] -match 'S2-P5 cell selected index=') { $markerIndices += $i }
    }
    $expectedModes = @(
        'direct',
        'dlss-dlaa', 'dlss-quality', 'dlss-balanced', 'dlss-performance',
        'dlss-ultra-performance',
        'rr-dlaa', 'rr-quality', 'rr-balanced', 'rr-performance',
        'rr-ultra-performance')
    $periods = @{
        'dlss-dlaa' = 8; 'dlss-quality' = 18; 'dlss-balanced' = 24;
        'dlss-performance' = 32; 'dlss-ultra-performance' = 72;
        'rr-dlaa' = 64; 'rr-quality' = 64; 'rr-balanced' = 64;
        'rr-performance' = 64; 'rr-ultra-performance' = 64
    }
    $failures = @()
    if ($markerIndices.Count -ne 66) { $failures += "expected 66 cells, observed $($markerIndices.Count)" }
    $activeContracts = @{}
    $cells = @()
    for ($markerOrdinal = 0; $markerOrdinal -lt $markerIndices.Count; ++$markerOrdinal) {
        $start = $markerIndices[$markerOrdinal]
        $end = if ($markerOrdinal + 1 -lt $markerIndices.Count) {
            $markerIndices[$markerOrdinal + 1] - 1
        } else { $allLines.Count - 1 }
        $markerLine = $allLines[$start]
        if ($markerLine -notmatch 'index=(\d+) extent-case=(\d+) mode=([^ ]+) ev=(-?\d+(?:\.\d+)?)') {
            $failures += "could not parse marker ordinal $markerOrdinal"
            continue
        }
        $index = [int]$Matches[1]
        $extentCase = [int]$Matches[2]
        $mode = $Matches[3]
        $ev = [double]$Matches[4]
        $segment = @($allLines[$start..$end])
        $localIndex = $index % 33
        $modeIndex = [Math]::Floor($localIndex / 3)
        $evIndex = $localIndex % 3
        $expectedMode = $expectedModes[$modeIndex]
        if ($mode -ne $expectedMode) { $failures += "cell $index mode=$mode expected=$expectedMode" }
        $expectedEv = @(-2.0, 0.0, 2.0)[$evIndex]
        if ($ev -ne $expectedEv) { $failures += "cell $index ev=$ev expected=$expectedEv" }

        $history = @($segment | Where-Object { $_ -match '^\[frame\] history-key ' })
        $schedules = @($history | Where-Object { $_ -match ' event=schedule ' })
        $commits = @($history | Where-Object { $_ -match ' event=commit ' })
        $scheduleByViewport = @{}
        $resetReasons = @{}
        foreach ($line in $schedules) {
            if ($line -match 'viewport=(\d+).*reason_bits=0x([0-9A-Fa-f]+)') {
                $view = [int]$Matches[1]
                $viewKey = [string]$view
                if (!$scheduleByViewport.ContainsKey($viewKey)) { $scheduleByViewport[$viewKey] = 0 }
                ++$scheduleByViewport[$viewKey]
                $resetReasons[[string]$view] = '0x' + $Matches[2].ToUpperInvariant()
            }
        }
        $expectsReset = $evIndex -eq 0 -and !($extentCase -eq 0 -and $modeIndex -eq 0)
        foreach ($view in @(0, 1)) {
            $viewKey = [string]$view
            $count = if ($scheduleByViewport.ContainsKey($viewKey)) {
                $scheduleByViewport[$viewKey]
            } else { 0 }
            $expectedCount = if ($expectsReset) { 1 } else { 0 }
            if ($count -ne $expectedCount) {
                $failures += "cell $index viewport $view reset count=$count expected=$expectedCount"
            }
        }

        $viewportExtents = @{}
        foreach ($line in $commits) {
            if ($line -match 'viewport=(\d+).*render=(\d+x\d+) output=(\d+x\d+)') {
                $viewportExtents[[string][int]$Matches[1]] = [ordered]@{
                    render = $Matches[2]
                    output = $Matches[3]
                }
            }
        }
        foreach ($view in @(0, 1)) {
            if (!$viewportExtents.ContainsKey([string]$view)) {
                $failures += "cell $index missing viewport $view extent/commit"
            }
        }

        $contractLines = @($segment | Where-Object { $_ -match 'active-contract viewport=' })
        foreach ($line in $contractLines) {
            if ($line -match 'viewport=(\d+)') { $activeContracts[[string][int]$Matches[1]] = $line }
        }
        $cellContracts = @{}
        $contractRecords = @()
        $evaluations = @($segment | Where-Object {
            $_ -match '^\[frame\] dlss-trace .* outcome=evaluated '
        })
        $jitterCommits = @($segment | Where-Object {
            $_ -match '^\[frame\] jitter-trace .* event=commit '
        })
        $tokenRecords = @()
        foreach ($line in $evaluations) {
            if ($line -match 'app_serial=(\d+).*viewport=(\d+).*token=(\d+)') {
                $tokenRecords += [pscustomobject]@{
                    app_serial = [uint64]$Matches[1]
                    viewport = [int]$Matches[2]
                    token = [uint32]$Matches[3]
                }
            }
        }
        $sharedTokenFrames = @($tokenRecords | Group-Object app_serial | Where-Object {
            $views = @($_.Group | Select-Object -ExpandProperty viewport -Unique)
            $tokens = @($_.Group | Select-Object -ExpandProperty token -Unique)
            $views.Count -eq 2 -and $tokens.Count -eq 1
        }).Count

        if ($mode -eq 'direct') {
            if ($evaluations.Count -ne 0 -or $jitterCommits.Count -ne 0) {
                $failures += "cell $index direct path unexpectedly evaluated reconstruction"
            }
        } else {
            $expectedPeriod = [int]$periods[$mode]
            foreach ($view in @(0, 1)) {
                $viewEvaluations = @($tokenRecords | Where-Object viewport -eq $view)
                if ($viewEvaluations.Count -lt 10) {
                    $failures += "cell $index viewport $view has only $($viewEvaluations.Count) evaluations"
                }
                $viewJitter = @($jitterCommits | Where-Object {
                    $_ -match "viewport=$view "
                })
                $badPeriods = @($viewJitter | Where-Object {
                    $_ -notmatch "period=$expectedPeriod "
                })
                if ($viewJitter.Count -lt 10 -or $badPeriods.Count -ne 0) {
                    $failures += "cell $index viewport $view jitter period/count failed"
                }
                if (!$activeContracts.ContainsKey([string]$view)) {
                    $failures += "cell $index viewport $view missing active contract"
                    continue
                }
                $contract = $activeContracts[[string]$view]
                $cellContracts[[string]$view] = $contract
                if ($contract -notmatch 'feature=([^ ]+) quality=([^ ]+) render=(\d+x\d+) output=(\d+x\d+) source=([^ ]+) motion-format=(\d+) depth-format=(\d+) states=([^ ]+) motion-scale=([^ ]+) tag-extents=explicit lifetimes=valid-until-present rr-no-arbitrary-drs=([^ ]+)') {
                    $failures += "cell $index viewport $view could not parse active contract"
                    continue
                }
                $feature = $Matches[1]
                $quality = $Matches[2]
                $contractRender = $Matches[3]
                $contractOutput = $Matches[4]
                $source = $Matches[5]
                $motionFormat = [int]$Matches[6]
                $depthFormat = [int]$Matches[7]
                $states = $Matches[8]
                $motionScale = $Matches[9]
                $rrNoDrs = $Matches[10]
                $expectedFeature = if ($mode.StartsWith('rr-')) { 'rr' } else { 'dlss' }
                $expectedQuality = $mode.Substring($mode.IndexOf('-') + 1)
                if ($feature -ne $expectedFeature -or $quality -ne $expectedQuality `
                    -or $source -ne 'sdk' -or $motionFormat -ne 34 -or $depthFormat -ne 45 `
                    -or $states -match '4294967295' -or $motionScale -ne '-0.5,0.5' `
                    -or ($expectedFeature -eq 'rr' -and $rrNoDrs -ne 'true')) {
                    $failures += "cell $index viewport $view active contract invariant failed"
                }
                if ($viewportExtents.ContainsKey([string]$view)) {
                    $extent = $viewportExtents[[string]$view]
                    if ($contractRender -ne $extent.render -or $contractOutput -ne $extent.output) {
                        $failures += "cell $index viewport $view contract/history extent mismatch"
                    }
                    if ($quality -eq 'dlaa' -and $contractRender -ne $contractOutput) {
                        $failures += "cell $index viewport $view DLAA is not native"
                    }
                }
                $contractRecords += [ordered]@{
                    viewport = $view
                    feature = $feature
                    quality = $quality
                    render_extent = $contractRender
                    output_extent = $contractOutput
                    source = $source
                    motion_format = $motionFormat
                    depth_format = $depthFormat
                    states = $states
                    motion_scale = $motionScale
                    tags = 'explicit'
                    lifetime = 'valid-until-present'
                    rr_no_arbitrary_drs = $rrNoDrs
                }
            }
            if ($sharedTokenFrames -lt 5) {
                $failures += "cell $index has only $sharedTokenFrames shared dual-view token frames"
            }
        }

        $screenshot = if ($capturedCells.ContainsKey($index)) { $capturedCells[$index] } else { $null }
        if ($ev -eq 0.0 -and $null -eq $screenshot) { $failures += "cell $index is missing EV0 screenshot" }
        $cells += [ordered]@{
            index = $index
            extent_case = $extentCase
            mode = $mode
            authored_ev = $ev
            expected_display_scale = [Math]::Pow(2.0, $ev)
            expects_transition_reset = $expectsReset
            reset_counts = $scheduleByViewport
            reset_reasons = $resetReasons
            viewport_extents = $viewportExtents
            jitter_period = if ($mode -eq 'direct') { $null } else { [int]$periods[$mode] }
            evaluated_frames = $evaluations.Count
            shared_dual_view_token_frames = $sharedTokenFrames
            token_min = if ($tokenRecords.Count -ne 0) {
                ($tokenRecords | Measure-Object token -Minimum).Minimum
            } else { $null }
            token_max = if ($tokenRecords.Count -ne 0) {
                ($tokenRecords | Measure-Object token -Maximum).Maximum
            } else { $null }
            contracts = $contractRecords
            screenshot = $screenshot
        }
    }

    $observedModeExtent = @($cells | ForEach-Object { "$($_.extent_case)/$($_.mode)" } |
        Sort-Object -Unique)
    if ($observedModeExtent.Count -ne 22) {
        $failures += "expected 22 mode/extent combinations, observed $($observedModeExtent.Count)"
    }
    foreach ($extentCase in @(0, 1)) {
        $outputs = @($cells | Where-Object { $_.extent_case -eq $extentCase } |
            ForEach-Object { $_.viewport_extents.GetEnumerator() } |
            ForEach-Object { $_.Value.output } | Sort-Object -Unique)
        if ($outputs.Count -lt 2) {
            $failures += "extent case $extentCase did not retain unequal Scene/Game extents"
        }
    }

    $runtimeFailures = @($allLines | Where-Object {
        $_ -match 'S2-P4 .*contract failed|tagging-failed|options-failed|evaluate-failed|outcome=failed|unsupported exposed|D3D12 device was removed|DXGI_ERROR_DEVICE_REMOVED|\[error\] \[dlss\]|LIVE OBJECT'
    })
    if ($runtimeFailures.Count -ne 0) {
        $failures += "observed $($runtimeFailures.Count) SDK/D3D12/runtime failure records"
    }
    $timingSamples = @(Import-Csv -LiteralPath $timings)
    $timingSummary = [ordered]@{
        frame_gpu_span = Get-TimingSummary $timingSamples 'frame_gpu_span_ms'
        path_tracer = Get-TimingSummary $timingSamples 'path_tracer_ms'
        dlss_evaluate = Get-TimingSummary $timingSamples 'dlss_evaluate_ms'
        cpu_frame = Get-TimingSummary $timingSamples 'cpu_frame_ms'
        cpu_render = Get-TimingSummary $timingSamples 'cpu_render_ms'
    }
    $result = [ordered]@{
        record_type = 's2p5_full_reconstruction_mode_matrix'
        schema_version = 1
        source_snapshot = '9b65f29127b818abaa142fbc851c157b409df02c'
        implementation_revision = $revision
        project = $projectPath
        matrix = [ordered]@{
            extent_cases = 2
            modes = $expectedModes
            exposures_ev = @(-2, 0, 2)
            expected_cells = 66
            observed_cells = $cells.Count
            unequal_scene_game_viewports = $true
            subpixel_camera_step_world = 0.00025
            fine_geometry_scene = 'DEMO-SCENE detailed imported geometry'
        }
        cells = $cells
        screenshots = @($capturedCells.Values | Sort-Object)
        sdk_d3d12_diagnostic_matches = $runtimeFailures.Count
        sdk_d3d12_diagnostics = $runtimeFailures
        timing_samples = $timingSamples.Count
        timing_summary = $timingSummary
        timings = $timings
        stdout = $stdout
        stderr = $stderr
        failures = $failures
        result = if ($failures.Count -eq 0) { 'PASS' } else { 'FAIL' }
    }
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifest -Encoding utf8
    if ($failures.Count -ne 0) {
        throw "S2-P5 mode matrix gate failed with $($failures.Count) issue(s); inspect $manifest."
    }
    Write-Host "S2-P5 mode matrix PASS: $outputRoot" -ForegroundColor Green
}
finally {
    if ($null -ne $process -and $null -ne (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $process.Id -Force
    }
    foreach ($name in $names) {
        if ($null -eq $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
}
