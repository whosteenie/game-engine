# Captures the S2-P4 active extent/tag/reset matrix on the real Streamline runtime.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s2p4/active-matrix',
    [ValidateRange(30, 180)][int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot $OutputDirectory) | Out-Null
$outputRoot = (Resolve-Path (Join-Path $repoRoot $OutputDirectory)).Path
$stdout = Join-Path $outputRoot 'stdout.log'
$stderr = Join-Path $outputRoot 'stderr.log'
$timings = Join-Path $outputRoot 'timings.csv'
Remove-Item -LiteralPath $stdout, $stderr, $timings -Force -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class S2P4WindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
'@

function Save-WindowCapture([System.Diagnostics.Process]$Process, [string]$Path) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $false }
    $rect = New-Object S2P4WindowCapture+Rect
    if (![S2P4WindowCapture]::GetWindowRect($handle, [ref]$rect)) { return $false }
    $width = $rect.R - $rect.L
    $height = $rect.B - $rect.T
    if ($width -le 0 -or $height -le 0) { return $false }
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try {
            if (![S2P4WindowCapture]::PrintWindow($handle, $hdc, 2)) { return $false }
        } finally { $graphics.ReleaseHdc($hdc) }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    return $true
}

function Wait-LogMarker([string]$Marker, [DateTime]$Deadline) {
    while ([DateTime]::UtcNow -lt $Deadline) {
        if ((Test-Path -LiteralPath $stderr) -and
            (Select-String -LiteralPath $stderr -SimpleMatch $Marker -Quiet)) { return }
        if ($null -ne $process -and $null -eq (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) {
            throw "Engine exited before log marker: $Marker"
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for log marker: $Marker"
}

$names = @(
    'GAME_ENGINE_AUTO_OPEN', 'GAME_ENGINE_FRAME_DEBUG', 'GAME_ENGINE_LOG',
    'GAME_ENGINE_S2P4_TRANSITIONS', 'GAME_ENGINE_S2P4_WINDOWED',
    'GAME_ENGINE_BENCHMARK_OUTPUT', 'GAME_ENGINE_BENCHMARK_WARMUP_SECONDS',
    'GAME_ENGINE_BENCHMARK_WARMUP_FRAMES', 'GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$process = $null

try {
    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'
    $env:GAME_ENGINE_S2P4_TRANSITIONS = '1'
    $env:GAME_ENGINE_S2P4_WINDOWED = '1'
    $env:GAME_ENGINE_BENCHMARK_OUTPUT = $timings
    $env:GAME_ENGINE_BENCHMARK_WARMUP_SECONDS = '0'
    $env:GAME_ENGINE_BENCHMARK_WARMUP_FRAMES = '1'
    $env:GAME_ENGINE_BENCHMARK_SAMPLE_FRAMES = '550'

    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

    Wait-LogMarker 'S2-P4 AOV selected motion-vectors' $deadline
    Start-Sleep -Milliseconds 1000
    $motionCapture = Join-Path $outputRoot 'motion-aov.png'
    $motionCaptured = Save-WindowCapture $process $motionCapture
    Wait-LogMarker 'S2-P4 AOV selected primary-depth' $deadline
    Start-Sleep -Milliseconds 1000
    $depthCapture = Join-Path $outputRoot 'depth-aov.png'
    $depthCaptured = Save-WindowCapture $process $depthCapture
    Wait-LogMarker 'S2-P4 AOV selected final' $deadline
    Start-Sleep -Milliseconds 300
    $finalCapture = Join-Path $outputRoot 'final.png'
    $finalCaptured = Save-WindowCapture $process $finalCapture
    Wait-LogMarker 'Shutting down NGX' $deadline

    $tupleLines = @(Select-String -LiteralPath $stderr -Pattern 'S2-P4 tuple selected feature=' |
        ForEach-Object Line)
    $contractLines = @(Select-String -LiteralPath $stderr -Pattern 'active-contract viewport=' |
        ForEach-Object Line)
    $requiredTuples = @(
        'dlss/dlaa', 'dlss/quality', 'dlss/balanced', 'dlss/performance',
        'dlss/ultra-performance', 'rr/dlaa', 'rr/quality', 'rr/balanced',
        'rr/performance', 'rr/ultra-performance')
    $observedTuples = @($contractLines | ForEach-Object {
        if ($_ -match 'feature=([^ ]+) quality=([^ ]+)') { "$($Matches[1])/$($Matches[2])" }
    } | Sort-Object -Unique)
    $missingTuples = @($requiredTuples | Where-Object { $_ -notin $observedTuples })
    $badMotion = @($contractLines | Where-Object { $_ -notmatch 'motion-format=34(?: |$)' })
    $badTags = @($contractLines | Where-Object {
        $_ -notmatch 'tag-extents=explicit lifetimes=valid-until-present' })
    $badSources = @($contractLines | Where-Object { $_ -notmatch 'source=sdk' })
    $outputs = @($contractLines | ForEach-Object {
        if ($_ -match 'output=(\d+x\d+)') { $Matches[1] }
    } | Sort-Object -Unique)
    $scheduleLines = @(Select-String -LiteralPath $stderr -Pattern (
        '^\[frame\] history-key .* event=schedule ') | ForEach-Object Line)
    $duplicateSchedules = @($scheduleLines | ForEach-Object {
        if ($_ -match 'app_serial=(\d+) viewport=(\d+)') { "$($Matches[1]):$($Matches[2])" }
    } | Group-Object | Where-Object Count -ne 1)
    $runtimeFailures = @(Select-String -LiteralPath $stderr -Pattern (
        'S2-P4 .*contract failed|tagging-failed|options-failed|evaluate-failed|' +
        'D3D12 device was removed|DXGI_ERROR_DEVICE_REMOVED|\[error\] \[dlss\]') |
        ForEach-Object Line)
    $screenshots = @()
    if ($motionCaptured) { $screenshots += $motionCapture }
    if ($depthCaptured) { $screenshots += $depthCapture }
    if ($finalCaptured) { $screenshots += $finalCapture }

    $result = [ordered]@{
        record_type = 's2p4_active_extent_tag_matrix'
        schema_version = 1
        project = $projectPath
        selected_tuple_records = $tupleLines.Count
        active_contract_records = $contractLines.Count
        observed_tuples = $observedTuples
        missing_tuples = $missingTuples
        output_extents = $outputs
        unsupported_motion_records = $badMotion.Count
        invalid_tag_contract_records = $badTags.Count
        non_sdk_active_records = $badSources.Count
        schedule_records = $scheduleLines.Count
        duplicate_schedule_frames = $duplicateSchedules.Count
        runtime_failures = $runtimeFailures
        screenshots = $screenshots
        timings = $timings
        stdout = $stdout
        stderr = $stderr
        result = 'PASS'
    }
    if ($missingTuples.Count -ne 0 -or $outputs.Count -lt 2 -or $badMotion.Count -ne 0 `
        -or $badTags.Count -ne 0 -or $badSources.Count -ne 0 `
        -or $duplicateSchedules.Count -ne 0 -or $runtimeFailures.Count -ne 0 `
        -or $screenshots.Count -ne 3 -or !(Test-Path -LiteralPath $timings)) {
        $result.result = 'FAIL'
    }
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
        Join-Path $outputRoot 's2-p4-active-manifest.json') -Encoding utf8
    if ($result.result -ne 'PASS') { throw 'S2-P4 active matrix gate failed; inspect manifest.' }
    Write-Host "S2-P4 active extent/tag matrix PASS: $outputRoot" -ForegroundColor Green
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
