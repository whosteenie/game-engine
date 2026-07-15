# Captures the S0-P2 application-frame / Streamline-token trace from a fresh editor process.
#
# Examples:
#   .\scripts\capture-s0p2-dlss-trace.ps1 -Project C:\Projects\Sample\Sample.gameproject -Mode Single
#   .\scripts\capture-s0p2-dlss-trace.ps1 -Project C:\Projects\Sample\Sample.gameproject -Mode Dual
#
# Dual requests the engine's opt-in GAME_ENGINE_AUTOMATION_DUAL_VIEW layout. It does not write the
# user's persisted ImGui layout; the layout is constructed in memory for this process only.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [ValidateSet('Single', 'Dual')]
    [string]$Mode = 'Dual',

    [ValidateRange(5, 300)]
    [int]$DurationSeconds = 30,

    [string]$BuildDir = 'build',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Debug',

    [string]$OutputDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDirectory '..')).Path
$projectPath = (Resolve-Path -LiteralPath $Project).Path
$exeDirectory = Join-Path $repoRoot (Join-Path $BuildDir $Config)
$enginePath = Join-Path $exeDirectory 'game-engine.exe'
if (-not (Test-Path -LiteralPath $enginePath)) {
    throw "Missing $enginePath. Build it first with: cmake --build $BuildDir --config $Config --target game-engine"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot ("diagnostics\\s0p2-dlss-trace\\{0}-{1}" -f $Mode.ToLowerInvariant(), $stamp)
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$stdoutPath = Join-Path $OutputDirectory 'stdout.log'
$stderrPath = Join-Path $OutputDirectory 'stderr.log'
$metadataPath = Join-Path $OutputDirectory 'capture.json'
$environmentNames = @('GAME_ENGINE_AUTO_OPEN', 'GAME_ENGINE_FRAME_DEBUG', 'GAME_ENGINE_LOG', 'GAME_ENGINE_AUTOMATION_DUAL_VIEW')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'
    if ($Mode -eq 'Dual') {
        $env:GAME_ENGINE_AUTOMATION_DUAL_VIEW = '1'
    }
    else {
        Remove-Item Env:GAME_ENGINE_AUTOMATION_DUAL_VIEW -ErrorAction SilentlyContinue
    }

    $process = Start-Process -FilePath $enginePath -WorkingDirectory $exeDirectory `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
    Start-Sleep -Seconds $DurationSeconds
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id
        $process.WaitForExit()
    }

    $trace = @(Select-String -LiteralPath $stderrPath -Pattern '^\[frame\] dlss-trace ' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Line })
    $frameBoundaries = @(Select-String -LiteralPath $stderrPath -Pattern '^\[frame\] application-frame ' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Line })
    $result = [ordered]@{
        mode = $Mode
        project = $projectPath
        duration_seconds = $DurationSeconds
        trace_records = $trace.Count
        application_frame_boundaries = $frameBoundaries.Count
        stdout = $stdoutPath
        stderr = $stderrPath
        note = 'A failed Streamline evaluation must be captured with a deliberately failing runtime fixture; this script never changes Streamline inputs to manufacture one.'
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath $metadataPath -Encoding utf8
    Write-Host "S0-P2 $Mode capture: $metadataPath" -ForegroundColor Green
    Write-Host "Trace records: $($trace.Count); application-frame boundaries: $($frameBoundaries.Count)"
    if ($trace.Count -eq 0) {
        Write-Warning 'No DLSS trace records were captured. Confirm the project has an active camera and DLSS/RR is available.'
    }
}
finally {
    foreach ($name in $environmentNames) {
        $value = $savedEnvironment[$name]
        if ($null -eq $value) {
            Remove-Item ("Env:{0}" -f $name) -ErrorAction SilentlyContinue
        }
        else {
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
}
