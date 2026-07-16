# Queries the real Streamline DLSS/RR optimal-settings matrix in S2-P2 shadow mode, then repeats it
# with the explicit forced-failure hook. No project is loaded and no planned extent is allocated.
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s2p2/sdk-matrix',
    [ValidateRange(10, 180)][int]$TimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
$outputRoot = Join-Path $repoRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$outputRoot = (Resolve-Path $outputRoot).Path

$environmentNames = @(
    'GAME_ENGINE_S2P2_QUERY_MATRIX_OUTPUT',
    'GAME_ENGINE_S2P2_FORCE_QUERY_FAILURE',
    'GAME_ENGINE_LOG')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Invoke-ExtentMatrix([string]$Name, [bool]$ForceFailure) {
    $matrix = Join-Path $outputRoot "$Name-matrix.json"
    $stdout = Join-Path $outputRoot "$Name.stdout.log"
    $stderr = Join-Path $outputRoot "$Name.stderr.log"
    Remove-Item -LiteralPath $matrix, $stdout, $stderr -Force -ErrorAction SilentlyContinue

    $env:GAME_ENGINE_S2P2_QUERY_MATRIX_OUTPUT = $matrix
    $env:GAME_ENGINE_LOG = '1'
    if ($ForceFailure) {
        $env:GAME_ENGINE_S2P2_FORCE_QUERY_FAILURE = '1'
    } else {
        Remove-Item Env:GAME_ENGINE_S2P2_FORCE_QUERY_FAILURE -ErrorAction SilentlyContinue
    }

    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq (Get-Process -Id $process.Id -ErrorAction SilentlyContinue) `
            -or (Test-Path -LiteralPath $matrix)) { break }
        Start-Sleep -Milliseconds 250
    }
    if (!(Test-Path -LiteralPath $matrix)) {
        if ($null -ne (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) {
            Stop-Process -Id $process.Id -Force
        }
        throw "$Name did not produce $matrix within $TimeoutSeconds seconds."
    }
    # PowerShell 5 can retain a stale Process object for this GUI executable after it has exited.
    # The engine writes the matrix before orderly Streamline shutdown, so use the shutdown log as
    # the completion boundary instead of HasExited/WaitForExit.
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Test-Path -LiteralPath $stderr) -and (Select-String -LiteralPath $stderr `
                -Pattern 'Shutting down NGX' -Quiet)) { break }
        Start-Sleep -Milliseconds 250
    }
    if (!(Select-String -LiteralPath $stderr -Pattern 'Shutting down NGX' -Quiet)) {
        throw "$Name did not reach orderly Streamline shutdown after writing its matrix."
    }

    $data = Get-Content -LiteralPath $matrix -Raw | ConvertFrom-Json
    if ($data.entries.Count -ne 20) { throw "$Name returned $($data.entries.Count), expected 20." }
    if ($ForceFailure) {
        $fallbacks = @($data.entries | Where-Object {
            $_.source -eq 'explicit-fallback' -and $_.fallback_reason -eq 'forced-query-failure' })
        if ($fallbacks.Count -ne 20) { throw "$Name has $($fallbacks.Count) explicit forced fallbacks." }
        $nativeRr = @($data.entries | Where-Object {
            $_.feature -eq 'rr' -and $_.rr_no_arbitrary_drs `
                -and $_.recommended[0] -eq $_.output[0] -and $_.recommended[1] -eq $_.output[1] })
        if ($nativeRr.Count -ne 10) { throw "$Name has $($nativeRr.Count) native RR fallbacks." }
        $expectedWarnings = @(Select-String -LiteralPath $stderr `
            -Pattern 'source=explicit-fallback.*fallback-reason=forced-query-failure')
        if ($expectedWarnings.Count -ne 20) { throw "$Name logged $($expectedWarnings.Count) fallback warnings." }
    } else {
        $sdkEntries = @($data.entries | Where-Object { $_.source -eq 'sdk' })
        if ($sdkEntries.Count -ne 20) { throw "$Name has $($sdkEntries.Count) SDK recommendations." }
        $diagnostics = @(Select-String -LiteralPath $stderr -Pattern '\[(warn|error)\]' -CaseSensitive:$false)
        if ($diagnostics.Count -ne 0) { throw "$Name contains $($diagnostics.Count) warning/error records." }
    }

    return [ordered]@{
        name = $Name
        forced_failure = $ForceFailure
        entries = $data.entries.Count
        sdk_recommendations = @($data.entries | Where-Object source -eq 'sdk').Count
        explicit_fallbacks = @($data.entries | Where-Object source -eq 'explicit-fallback').Count
        matrix = $matrix
        stdout = $stdout
        stderr = $stderr
        process_status = 'orderly-streamline-shutdown'
    }
}

try {
    $sdk = Invoke-ExtentMatrix 'sdk' $false
    $forced = Invoke-ExtentMatrix 'forced-failure' $true
    [ordered]@{
        record_type = 's2p2_extent_query_gate'
        schema_version = 1
        allocation_mode = 'shadow-active-unchanged-until-s2p4'
        runs = @($sdk, $forced)
        result = 'PASS'
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
        Join-Path $outputRoot 's2-p2-extent-manifest.json') -Encoding utf8
    Write-Host "S2-P2 extent query matrix PASS: $outputRoot" -ForegroundColor Green
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
