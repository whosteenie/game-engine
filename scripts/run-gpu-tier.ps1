[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][ValidateSet(1, 2, 4, 5)][int]$Tier,
    [string]$LogPath,
    [int]$AssertionTimeoutSeconds = 360,
    [int]$TeardownGraceSeconds = 10
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$Executable = (Resolve-Path -LiteralPath $Executable).Path
$workingDirectory = Split-Path -Parent $Executable
if (-not $LogPath) {
    $LogPath = Join-Path $workingDirectory "gpu-tier-$Tier.log"
} elseif (-not [IO.Path]::IsPathRooted($LogPath)) {
    $LogPath = Join-Path (Get-Location) $LogPath
}
$LogPath = [IO.Path]::GetFullPath($LogPath)
$logDirectory = Split-Path -Parent $LogPath
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null

$expectedPasses = @{ 1 = 9; 2 = 11; 4 = 2; 5 = 6 }[$Tier]
$stdoutPath = "$LogPath.stdout.tmp"
$stderrPath = "$LogPath.stderr.tmp"
Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue

$process = Start-Process `
    -FilePath $Executable `
    -ArgumentList "--tier=$Tier" `
    -WorkingDirectory $workingDirectory `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -WindowStyle Hidden `
    -PassThru

$watch = [Diagnostics.Stopwatch]::StartNew()
$assertionsCompletedAt = $null
$terminatedForTeardown = $false
try {
    while (-not $process.HasExited) {
        $stdout = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -Raw -LiteralPath $stdoutPath } else { "" }
        $stderr = if (Test-Path -LiteralPath $stderrPath) { Get-Content -Raw -LiteralPath $stderrPath } else { "" }
        $combined = "$stdout`n$stderr"
        $passCount = ([regex]::Matches($combined, '(?m)^\[PASS\] ')).Count
        $hasFailure = $combined -match '(?m)^\[(FAIL|FAILED)\]|^FAIL:'

        if ($hasFailure) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "GPU tier $Tier reported an assertion failure"
        }
        if ($passCount -ge $expectedPasses -and $null -eq $assertionsCompletedAt) {
            $assertionsCompletedAt = $watch.Elapsed
            Write-Output "GPU tier $Tier completed $passCount/$expectedPasses assertions; allowing ${TeardownGraceSeconds}s for teardown."
        }
        if ($null -ne $assertionsCompletedAt -and
            ($watch.Elapsed - $assertionsCompletedAt).TotalSeconds -ge $TeardownGraceSeconds) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $terminatedForTeardown = $true
            break
        }
        if ($watch.Elapsed.TotalSeconds -ge $AssertionTimeoutSeconds) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "GPU tier $Tier did not complete assertions within ${AssertionTimeoutSeconds}s"
        }
        Start-Sleep -Milliseconds 250
        $process.Refresh()
    }
}
finally {
    if (-not $process.HasExited) {
        $process.WaitForExit(5000) | Out-Null
    }
    # Windows PowerShell 5 can retain a null ExitCode after a polled process exits normally.
    $process.Refresh()
    $watch.Stop()
    $stdout = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -Raw -LiteralPath $stdoutPath } else { "" }
    $stderr = if (Test-Path -LiteralPath $stderrPath) { Get-Content -Raw -LiteralPath $stderrPath } else { "" }
    $combined = "$stdout`n$stderr"
    $passCount = ([regex]::Matches($combined, '(?m)^\[PASS\] ')).Count
    $hasFailure = $combined -match '(?m)^\[(FAIL|FAILED)\]|^FAIL:'
    $result = if ($hasFailure) {
        "failed"
    } elseif ($terminatedForTeardown) {
        "assertions_passed_teardown_hung"
    } elseif ($passCount -ge $expectedPasses) {
        "passed"
    } else {
        "failed"
    }
    $processExit = if ($terminatedForTeardown) {
        "terminated"
    } elseif ($null -eq $process.ExitCode) {
        "unavailable"
    } else {
        "$($process.ExitCode)"
    }
    @(
        "tier=$Tier expected_passes=$expectedPasses observed_passes=$passCount"
        "result=$result process_exit=$processExit elapsed_seconds=$([math]::Round($watch.Elapsed.TotalSeconds, 3))"
        "--- stdout ---"
        $stdout
        "--- stderr ---"
        $stderr
    ) | Out-File -LiteralPath $LogPath -Encoding utf8
    Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
}

if ($hasFailure) {
    throw "GPU tier $Tier reported an assertion failure (see $LogPath)"
}
if ($passCount -lt $expectedPasses) {
    throw "GPU tier $Tier completed only $passCount/$expectedPasses assertions (see $LogPath)"
}

Write-Output "GPU tier ${Tier}: $result ($passCount/$expectedPasses assertions, $([math]::Round($watch.Elapsed.TotalSeconds, 3))s)"
