# Runs named path-tracing GPU tests independently and preserves assertion evidence even when the
# known D3D12 test-host teardown stall prevents a clean process exit.
# Usage: .\scripts\run-pt-gpu-gate.ps1 -Config Debug -TimeoutSeconds 180

[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Debug",
    [int]$TimeoutSeconds = 180,
    [string]$ArtifactsDir = "artifacts/s1-p2-gpu-tests",
    [string[]]$Filter = @(
        "PtTransmissionGuideAlbedoBands",
        "PtTransmissionVirtualMotionOnOrbit",
        "PtStaticOffOriginOpaqueMotion",
        "PtTransmissionVirtualMotionLateralChecker",
        "PtTransmissionDiagnosticsOffEquivalence",
        "PtRestirStaticPreviousReceiverTargetAgreement"
    )
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($TimeoutSeconds -le 0) {
    throw "TimeoutSeconds must be positive."
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$ExeDir = Join-Path $RepoRoot (Join-Path $BuildDir $Config)
$ExePath = Join-Path $ExeDir "d3d12-render-tests.exe"
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "Missing GPU test executable: $ExePath"
}

$ResolvedArtifactsDir = Join-Path $RepoRoot $ArtifactsDir
New-Item -ItemType Directory -Force -Path $ResolvedArtifactsDir | Out-Null
$RunId = Get-Date -Format "yyyyMMdd-HHmmss"
$ManifestPath = Join-Path $ResolvedArtifactsDir "s1-p2-gpu-gate-$RunId.json"
$Results = [System.Collections.Generic.List[object]]::new()

foreach ($TestName in $Filter) {
    $StdoutPath = Join-Path $ResolvedArtifactsDir "$RunId-$TestName.stdout.log"
    $StderrPath = Join-Path $ResolvedArtifactsDir "$RunId-$TestName.stderr.log"
    $Arguments = @("--tier=5", "--filter=$TestName")
    Write-Host "==> $TestName" -ForegroundColor Cyan
    $Process = Start-Process -FilePath $ExePath -ArgumentList $Arguments -WorkingDirectory $ExeDir `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath -PassThru
    $Exited = $Process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $Exited) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
    }
    else {
        # Refresh after WaitForExit so ExitCode is materialized consistently by PowerShell.
        $Process.Refresh()
    }

    $Lines = @()
    if (Test-Path -LiteralPath $StdoutPath) { $Lines += Get-Content -LiteralPath $StdoutPath }
    if (Test-Path -LiteralPath $StderrPath) { $Lines += Get-Content -LiteralPath $StderrPath }
    $AssertionPassed = @($Lines | Where-Object { $_ -eq "[PASS] $TestName" }).Count -gt 0
    $Result = [pscustomobject]@{
        test = $TestName
        command = "$ExePath --tier=5 --filter=$TestName"
        assertionPassed = $AssertionPassed
        exitedCleanly = $Exited
        exitCode = if ($Exited) { [int]$Process.ExitCode } else { $null }
        teardownTimedOut = -not $Exited
        stdout = $StdoutPath
        stderr = $StderrPath
    }
    $Results.Add($Result)
    $State = if ($AssertionPassed) { "ASSERTIONS PASS" } else { "ASSERTIONS FAILED/NOT REACHED" }
    $Teardown = if ($Exited) { "clean exit ($($Process.ExitCode))" } else { "teardown timeout after ${TimeoutSeconds}s" }
    Write-Host "    $State; $Teardown"
}

$Manifest = [pscustomobject]@{
    runId = $RunId
    executable = $ExePath
    tier = 5
    timeoutSeconds = $TimeoutSeconds
    results = $Results
}
$Manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ManifestPath -Encoding utf8
Write-Host "Manifest: $ManifestPath"

if (@($Results | Where-Object { -not $_.assertionPassed }).Count -ne 0) {
    exit 1
}

exit 0
