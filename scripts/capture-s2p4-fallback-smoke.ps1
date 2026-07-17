[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'artifacts/s2p4/fallback-smoke',
    [ValidateRange(20, 120)][int]$TimeoutSeconds = 60
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build first." }
New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot $OutputDirectory) | Out-Null
$outputRoot = (Resolve-Path (Join-Path $repoRoot $OutputDirectory)).Path
$stdout = Join-Path $outputRoot 'stdout.log'
$stderr = Join-Path $outputRoot 'stderr.log'
$names = @('GAME_ENGINE_AUTO_OPEN','GAME_ENGINE_FRAME_DEBUG','GAME_ENGINE_LOG',
    'GAME_ENGINE_S2P4_FALLBACK_SMOKE','GAME_ENGINE_S2P4_WINDOWED',
    'GAME_ENGINE_S2P2_FORCE_QUERY_FAILURE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$process = $null
try {
    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'
    $env:GAME_ENGINE_S2P4_FALLBACK_SMOKE = '1'
    $env:GAME_ENGINE_S2P4_WINDOWED = '1'
    $env:GAME_ENGINE_S2P2_FORCE_QUERY_FAILURE = '1'
    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Test-Path -LiteralPath $stderr) -and
            (Select-String -LiteralPath $stderr -SimpleMatch 'Shutting down NGX' -Quiet)) { break }
        Start-Sleep -Milliseconds 100
    }
    if (!(Select-String -LiteralPath $stderr -SimpleMatch 'Shutting down NGX' -Quiet)) {
        throw 'Fallback smoke did not reach orderly Streamline shutdown.'
    }
    $contracts = @(Select-String -LiteralPath $stderr -Pattern 'active-contract viewport=' |
        ForEach-Object Line)
    $dlss = @($contracts | Where-Object {
        $_ -match 'feature=dlss quality=performance' -and $_ -match 'source=explicit-fallback' })
    $rr = @($contracts | Where-Object {
        $_ -match 'feature=rr quality=performance' -and $_ -match 'source=explicit-fallback' `
            -and $_ -match 'rr-no-arbitrary-drs=true' -and
            $_ -match 'render=(\d+)x(\d+) output=\1x\2' })
    $failures = @(Select-String -LiteralPath $stderr -Pattern (
        'S2-P4 .*contract failed|tagging-failed|options-failed|evaluate-failed|' +
        'D3D12 device was removed|DXGI_ERROR_DEVICE_REMOVED|\[error\] \[dlss\]'))
    $result = [ordered]@{
        record_type = 's2p4_active_fallback_smoke'
        schema_version = 1
        dlss_fallback_contracts = $dlss.Count
        rr_native_fallback_contracts = $rr.Count
        runtime_failures = $failures.Count
        stdout = $stdout
        stderr = $stderr
        result = if ($dlss.Count -gt 0 -and $rr.Count -gt 0 -and $failures.Count -eq 0) { 'PASS' } else { 'FAIL' }
    }
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (
        Join-Path $outputRoot 's2-p4-fallback-manifest.json') -Encoding utf8
    if ($result.result -ne 'PASS') { throw 'S2-P4 fallback smoke failed; inspect manifest.' }
    Write-Host "S2-P4 fallback smoke PASS: $outputRoot" -ForegroundColor Green
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
