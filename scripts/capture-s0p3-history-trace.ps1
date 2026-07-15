# Captures the S0-P3 steady-state and existing-setter transition history trace from DEMO-SCENE.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [ValidateRange(15, 180)][int]$DurationSeconds = 45,
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptRoot = if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) { (Get-Location).Path } else { $PSScriptRoot }
$repoRoot = if (Test-Path -LiteralPath (Join-Path $scriptRoot 'CMakeLists.txt')) {
    (Resolve-Path $scriptRoot).Path
} else {
    (Resolve-Path (Join-Path $scriptRoot '..')).Path
}
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\\$Config\\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot ('diagnostics\\s0p3-history-trace\\demo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$saved = @{}
$names = @('GAME_ENGINE_AUTO_OPEN','GAME_ENGINE_FRAME_DEBUG','GAME_ENGINE_LOG','GAME_ENGINE_S0P3_TRANSITIONS')
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'
    $env:GAME_ENGINE_S0P3_TRANSITIONS = '1'
    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    Start-Sleep -Seconds $DurationSeconds
    if (!$process.HasExited) { Stop-Process -Id $process.Id; $process.WaitForExit() }
    $trace = @(Select-String -LiteralPath $stderr -Pattern '^\[frame\] history-trace ' | ForEach-Object Line)
    $owners = @('reconstruction','pt-reference-accumulation','restir-temporal','render-bloom','dlss-display-bloom')
$missing = @()
foreach ($owner in $owners) {
    if (@($trace | Where-Object { $_ -match ("owner=" + [regex]::Escape($owner) + " ") }).Count -eq 0) {
        $missing += $owner
    }
}
    $result = [ordered]@{ project=$projectPath; duration_seconds=$DurationSeconds; history_records=$trace.Count; owners=$owners; missing_owners=$missing; stdout=$stdout; stderr=$stderr; transition_frames='30 RR-off; 60 RR-on; 90 DLSS; 120 performance; 150 resize; 180 diagnostic-on; 210 diagnostic-off; 240 hybrid; 270 PT' }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'capture.json') -Encoding utf8
    Write-Host "S0-P3 capture: $OutputDirectory; history records: $($trace.Count); missing owners: $($missing -join ', ')"
    if ($trace.Count -eq 0 -or $missing.Count -ne 0) { exit 2 }
}
finally {
    foreach ($name in $names) { if ($null -eq $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue } else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') } }
}
