# Validates monotonic presented-image luminance for the fresh S2-P5 exposure matrix.
[CmdletBinding()]
param(
    [string]$InputDirectory = 'artifacts/s2p5/exposure-patches',
    [string]$OutputManifest = 'artifacts/s2p5/s2-p5-exposure-validation.json'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$inputRoot = (Resolve-Path (Join-Path $repoRoot $InputDirectory)).Path
$sourceManifestPath = Join-Path $inputRoot 's2-p1-exposure-manifest.json'
if (!(Test-Path -LiteralPath $sourceManifestPath)) { throw "Missing $sourceManifestPath." }
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$source = Get-Content -LiteralPath $sourceManifestPath -Raw | ConvertFrom-Json
$records = @()
$failures = @()
foreach ($capture in $source.captures) {
    $png = [string]$capture.screenshot
    if (!(Test-Path -LiteralPath $png)) {
        $failures += "missing image for $($capture.mode) EV $($capture.exposure_ev)"
        continue
    }
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $ffmpegOutput = @(& $ffmpeg -hide_banner -loglevel info -i $png `
            -vf 'signalstats,metadata=print:key=lavfi.signalstats.YAVG' `
            -frames:v 1 -f null - 2>&1)
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    $match = $ffmpegOutput | Select-String 'lavfi.signalstats.YAVG=' | Select-Object -Last 1
    if ($null -eq $match -or $match.Line -notmatch 'YAVG=([0-9.]+)') {
        $failures += "could not measure image luminance for $($capture.mode) EV $($capture.exposure_ev)"
        continue
    }
    $records += [ordered]@{
        mode = [string]$capture.mode
        authored_ev = [double]$capture.exposure_ev
        expected_display_scale = [double]$capture.display_scale
        yavg_8bit = [double]$Matches[1]
        sha256 = (Get-FileHash -LiteralPath $png -Algorithm SHA256).Hash
        image = $png
        manifest = [string]$capture.manifest
        timings = [string]$capture.timings
        stdout = [string]$capture.stdout
        stderr = [string]$capture.stderr
        settled_frames = [int]$capture.settled_frames
        sdk_d3d12_diagnostic_matches = [int]$capture.sdk_d3d12_diagnostic_matches
    }
}

$modeResults = @()
foreach ($mode in @($records | ForEach-Object mode | Sort-Object -Unique)) {
    $modeRecords = @($records | Where-Object mode -eq $mode)
    $minus = @($modeRecords | Where-Object authored_ev -eq -2)
    $zero = @($modeRecords | Where-Object authored_ev -eq 0)
    $plus = @($modeRecords | Where-Object authored_ev -eq 2)
    $complete = $minus.Count -eq 1 -and $zero.Count -eq 1 -and $plus.Count -eq 1
    $monotonic = $complete -and $minus[0].yavg_8bit -lt $zero[0].yavg_8bit `
        -and $zero[0].yavg_8bit -lt $plus[0].yavg_8bit
    if (!$monotonic) { $failures += "$mode presented luminance is not strictly monotonic" }
    $modeResults += [ordered]@{
        mode = $mode
        ev_minus_2_yavg = if ($complete) { $minus[0].yavg_8bit } else { $null }
        ev_0_yavg = if ($complete) { $zero[0].yavg_8bit } else { $null }
        ev_plus_2_yavg = if ($complete) { $plus[0].yavg_8bit } else { $null }
        strictly_monotonic = $monotonic
    }
}
if ($records.Count -ne 33) { $failures += "expected 33 images, measured $($records.Count)" }
if (@($modeResults | Where-Object { !$_.strictly_monotonic }).Count -ne 0) {
    $failures += 'one or more mode exposure triples failed monotonicity'
}
if ([int]$source.sdk_d3d12_diagnostic_matches -ne 0) {
    $failures += 'source exposure matrix contains SDK/D3D12 diagnostics'
}

$outputPath = Join-Path $repoRoot $OutputManifest
New-Item -ItemType Directory -Force -Path (Split-Path $outputPath) | Out-Null
$result = [ordered]@{
    record_type = 's2p5_exposure_image_validation'
    schema_version = 1
    source_snapshot = '9b65f29127b818abaa142fbc851c157b409df02c'
    source_manifest = $sourceManifestPath
    image_records = $records
    mode_results = $modeResults
    expected_cpu_patch_values = [ordered]@{
        ev_minus_2 = 0.03125
        ev_0 = 0.125
        ev_plus_2 = 0.5
        endpoint_ratio = 16.0
    }
    failures = $failures
    result = if ($failures.Count -eq 0) { 'PASS' } else { 'FAIL' }
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding utf8
if ($failures.Count -ne 0) {
    throw "S2-P5 exposure image validation failed; inspect $outputPath."
}
Write-Host "S2-P5 exposure image validation PASS: $outputPath" -ForegroundColor Green
