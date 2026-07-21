# Automated cold-process project-load benchmark.
#
# Each sample starts a fresh editor process, opens the selected project without user input, writes
# phase timings to JSON, then closes after the first Scene View composite has been recorded.
#
# Examples:
#   .\scripts\benchmark-project-load.ps1 -Project C:\Projects\Sample\Sample.gameproject
#   .\scripts\benchmark-project-load.ps1 -Project .\test-scenes\heavy.gameproject -Runs 5

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [ValidateRange(1, 30)]
    [int]$Runs = 3,

    [string]$BuildDir = "build",

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    [string]$OutputDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-Stats {
    param([double[]]$Values)

    if ($Values.Count -eq 0) {
        return $null
    }

    $sorted = @($Values | Sort-Object)
    $middle = [int][math]::Floor($sorted.Count / 2)
    $median = if (($sorted.Count % 2) -eq 0) {
        ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
    }
    else {
        $sorted[$middle]
    }

    return [ordered]@{
        samples = $sorted.Count
        min_ms  = [math]::Round($sorted[0], 3)
        median_ms = [math]::Round($median, 3)
        mean_ms = [math]::Round((($sorted | Measure-Object -Average).Average), 3)
        max_ms = [math]::Round($sorted[$sorted.Count - 1], 3)
    }
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDirectory "..")).Path
$projectPath = (Resolve-Path -LiteralPath $Project).Path
$exeDirectory = Join-Path $repoRoot (Join-Path $BuildDir $Config)
$enginePath = Join-Path $exeDirectory "game-engine.exe"

if (-not (Test-Path -LiteralPath $enginePath)) {
    throw "Missing $enginePath. Build it first with: cmake --build $BuildDir --config $Config --target game-engine"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $safeProjectName = [IO.Path]::GetFileNameWithoutExtension($projectPath) -replace '[^a-zA-Z0-9._-]', '_'
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path $repoRoot ("diagnostics\project-load-benchmarks\{0}-{1}" -f $safeProjectName, $stamp)
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$environmentNames = @(
    "GAME_ENGINE_AUTO_OPEN",
    "GAME_ENGINE_AUTO_OPEN_DEFERRED",
    "GAME_ENGINE_PROJECT_LOAD_BENCHMARK_OUTPUT",
    "GAME_ENGINE_BENCHMARK_OUTPUT"
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

$samples = New-Object System.Collections.Generic.List[object]

try {
    for ($run = 1; $run -le $Runs; ++$run) {
        $resultPath = Join-Path $OutputDirectory ("run-{0:D2}.json" -f $run)
        $stdoutPath = Join-Path $OutputDirectory ("run-{0:D2}.stdout.log" -f $run)
        $stderrPath = Join-Path $OutputDirectory ("run-{0:D2}.stderr.log" -f $run)

        $env:GAME_ENGINE_AUTO_OPEN = $projectPath
        Remove-Item Env:GAME_ENGINE_AUTO_OPEN_DEFERRED -ErrorAction SilentlyContinue
        $env:GAME_ENGINE_PROJECT_LOAD_BENCHMARK_OUTPUT = $resultPath
        Remove-Item Env:GAME_ENGINE_BENCHMARK_OUTPUT -ErrorAction SilentlyContinue

        Write-Host ("[{0}/{1}] Opening {2}" -f $run, $Runs, [IO.Path]::GetFileName($projectPath)) -ForegroundColor Cyan
        $stopwatch = [Diagnostics.Stopwatch]::StartNew()
        try {
            # The engine intentionally writes breadcrumbs and warnings to stderr. Start-Process
            # keeps those diagnostics in a file instead of allowing PowerShell's strict native
            # command handling to treat a normal benchmark run as a terminating error.
            $process = Start-Process `
                -FilePath $enginePath `
                -WorkingDirectory $exeDirectory `
                -RedirectStandardOutput $stdoutPath `
                -RedirectStandardError $stderrPath `
                -Wait `
                -PassThru
            $exitCode = $process.ExitCode
        }
        finally {
            $stopwatch.Stop()
        }

        if (-not (Test-Path -LiteralPath $resultPath)) {
            throw "Run $run did not produce $resultPath (exit code $exitCode). See $stdoutPath and $stderrPath."
        }

        $result = Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
        $sample = [ordered]@{
            run = $run
            process_exit_code = $exitCode
            process_total_ms = [math]::Round($stopwatch.Elapsed.TotalMilliseconds, 3)
            result = $result
        }
        $samples.Add($sample)

        $status = [string]$result.status
        Write-Host (
            "  {0}: engine {1:N1} ms, process {2:N1} ms" -f
            $status, [double]$result.total_ms, $stopwatch.Elapsed.TotalMilliseconds)
        if ($exitCode -ne 0 -or $status -ne "complete") {
            throw "Run $run failed (status '$status', exit code $exitCode). See $stdoutPath and $stderrPath."
        }
    }
}
finally {
    foreach ($name in $environmentNames) {
        $value = $savedEnvironment[$name]
        if ($null -eq $value) {
            Remove-Item ("Env:{0}" -f $name) -ErrorAction SilentlyContinue
        }
        else {
            [Environment]::SetEnvironmentVariable($name, $value, "Process")
        }
    }
}

$phaseNames = @(
    $samples |
        ForEach-Object { $_.result.phases } |
        ForEach-Object { [string]$_.name } |
        Sort-Object -Unique
)

$phaseSummary = [ordered]@{}
foreach ($phaseName in $phaseNames) {
    $values = @(
        $samples |
            ForEach-Object {
                $_.result.phases |
                    Where-Object { $_.name -eq $phaseName } |
                    ForEach-Object { [double]$_.duration_ms }
            }
    )
    $phaseSummary[$phaseName] = Get-Stats -Values $values
}

$summary = [ordered]@{
    project = $projectPath
    configuration = $Config
    runs = $samples
    total_engine_ms = Get-Stats -Values @($samples | ForEach-Object { [double]$_.result.total_ms })
    total_process_ms = Get-Stats -Values @($samples | ForEach-Object { [double]$_.process_total_ms })
    phases = $phaseSummary
}

$summaryPath = Join-Path $OutputDirectory "summary.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8

Write-Host ""
Write-Host ("Project-load benchmark complete: {0}" -f $summaryPath) -ForegroundColor Green
Write-Host (
    "Engine load median: {0:N1} ms | Process median: {1:N1} ms" -f
    $summary.total_engine_ms.median_ms,
    $summary.total_process_ms.median_ms) -ForegroundColor White
Write-Host "Largest timed phases (median):" -ForegroundColor White
$phaseSummary.GetEnumerator() |
    Sort-Object { $_.Value.median_ms } -Descending |
    Select-Object -First 8 |
    ForEach-Object {
        Write-Host ("  {0,-42} {1,9:N1} ms" -f $_.Key, $_.Value.median_ms)
    }
