# Interactive test runner for game-engine (CPU + GPU suites).
# Usage (from repo root):
#   .\scripts\run-tests.ps1
#   .\scripts\run-tests.ps1 -Config Release -BuildDir build

[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Debug",
    [ValidateSet("", "Cpu", "Gpu", "All", "List", "Build")]
    [string]$Run = "",
    [int]$Tier = 1,
    [ValidateSet("", "Through", "Exact", "All", "Custom")]
    [string]$GpuMode = "Through",
    [string]$Tiers = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Convert-ProcessOutputLine {
    param($Item)

    if ($Item -is [System.Management.Automation.ErrorRecord]) {
        return $Item.ToString()
    }

    return [string]$Item
}

function Invoke-NativeCommand {
    param(
        [string]$ExePath,
        [string[]]$Arguments,
        [switch]$EmitLines
    )

    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $lines = New-Object System.Collections.Generic.List[string]

    try {
        & $ExePath @Arguments 2>&1 | ForEach-Object {
            $line = Convert-ProcessOutputLine $_
            [void]$lines.Add($line)
            if ($EmitLines) {
                Write-TestLine $line
            }
        }

        return [pscustomobject]@{
            Lines    = $lines.ToArray()
            ExitCode = $LASTEXITCODE
        }
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$ExeDir = Join-Path $RepoRoot (Join-Path $BuildDir $Config)

$EngineTestsExe = Join-Path $ExeDir "engine-tests.exe"
$DescriptorHeapExe = Join-Path $ExeDir "descriptor-heap-tests.exe"
$GpuRenderTestsExe = Join-Path $ExeDir "d3d12-render-tests.exe"

function Format-Duration {
    param([TimeSpan]$Elapsed)

    if ($Elapsed.TotalMinutes -ge 1.0) {
        $minutes = [int][Math]::Floor($Elapsed.TotalMinutes)
        $seconds = $Elapsed.Seconds
        return "${minutes}m ${seconds}s"
    }

    if ($Elapsed.TotalSeconds -ge 1.0) {
        return ("{0:N2}s" -f $Elapsed.TotalSeconds)
    }

    return ("{0}ms" -f [int]$Elapsed.TotalMilliseconds)
}

function Write-TestLine {
    param([string]$Line)

    if ($Line -match '\[breadcrumb\]') {
        if ($VerbosePreference -eq 'Continue') {
            Write-Host $Line -ForegroundColor DarkGray
        }
        return
    }

    if ($Line -match '^\[PASS\]') {
        Write-Host $Line -ForegroundColor Green
    }
    elseif ($Line -match '^\[FAIL\]') {
        Write-Host $Line -ForegroundColor Red
    }
    elseif ($Line -match '^(FAIL:|.*\bFAIL\b.*assertion)') {
        Write-Host $Line -ForegroundColor Red
    }
    elseif ($Line -match '^SKIP:') {
        Write-Host $Line -ForegroundColor Yellow
    }
    elseif ($Line -match '\[(framebuffer|hlsl|warn)\]') {
        Write-Host $Line -ForegroundColor DarkGray
    }
    else {
        Write-Host $Line
    }
}

function Get-TestCountsFromSummary {
    param([string[]]$Lines)

    foreach ($line in $Lines) {
        if ($line -match '^(\d+)/(\d+) tests passed') {
            return [pscustomobject]@{
                Passed = [int]$Matches[1]
                Total  = [int]$Matches[2]
            }
        }
    }

    return $null
}

function Invoke-TestExecutable {
    param(
        [string]$Label,
        [string]$ExePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $ExeDir
    )

    if (-not (Test-Path -LiteralPath $ExePath)) {
        Write-Host "Missing: $ExePath" -ForegroundColor Red
        Write-Host "Build it first (menu option 7) or run: cmake --build $BuildDir --config $Config --target <target>" -ForegroundColor DarkYellow
        return [pscustomobject]@{
            Label      = $Label
            Passed     = 0
            Total      = 0
            ExitCode   = 1
            Duration   = [TimeSpan]::Zero
            Skipped    = $true
        }
    }

    Write-Host ""
    Write-Host "==> $Label" -ForegroundColor Cyan
    if ($Arguments.Count -gt 0) {
        Write-Host ("    {0} {1}" -f (Split-Path -Leaf $ExePath), ($Arguments -join ' ')) -ForegroundColor DarkGray
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    Push-Location $WorkingDirectory
    try {
        $run = Invoke-NativeCommand -ExePath $ExePath -Arguments $Arguments -EmitLines
        $output = $run.Lines
        $exitCode = $run.ExitCode
    }
    finally {
        Pop-Location
        $stopwatch.Stop()
    }

    $counts = Get-TestCountsFromSummary -Lines $output
    if ($null -eq $counts) {
        $passMatches = @($output | Where-Object { $_ -match '^\[PASS\]' }).Count
        $failMatches = @($output | Where-Object { $_ -match '^\[FAIL\]' }).Count
        $counts = [pscustomobject]@{
            Passed = $passMatches
            Total  = $passMatches + $failMatches
        }
    }

  return [pscustomobject]@{
        Label    = $Label
        Passed   = $counts.Passed
        Total    = $counts.Total
        ExitCode = $exitCode
        Duration = $stopwatch.Elapsed
        Skipped  = $false
    }
}

function Show-Banner {
    Write-Host ""
    Write-Host "game-engine test runner" -ForegroundColor White
    Write-Host ("Config: {0}   Exe dir: {1}" -f $Config, $ExeDir) -ForegroundColor DarkGray
    Write-Host ("GPU suite: {0}" -f ($(if (Test-Path -LiteralPath $GpuRenderTestsExe) { "built" } else { "not built (enable GAME_ENGINE_BUILD_D3D12_RENDER_TESTS)" }))) -ForegroundColor DarkGray
}

function Get-CpuTestList {
    $tests = @()

    if (Test-Path -LiteralPath $EngineTestsExe) {
        Push-Location $ExeDir
        try {
            $run = Invoke-NativeCommand -ExePath $EngineTestsExe -Arguments @("--list")
            foreach ($line in $run.Lines) {
                if ($line -match '^(\S+)\s+\[cpu\]') {
                    $tests += [pscustomobject]@{ Name = $Matches[1]; Suite = "engine-tests"; Kind = "cpu" }
                }
            }
        }
        finally {
            Pop-Location
        }
    }

    if (Test-Path -LiteralPath $DescriptorHeapExe) {
        $tests += [pscustomobject]@{ Name = "fixed_descriptor_heap"; Suite = "descriptor-heap-tests"; Kind = "cpu" }
    }

    return $tests
}

function Get-GpuTierCliArguments {
    param(
        [ValidateSet("Through", "Exact", "All", "Custom")]
        [string]$Mode,
        [int]$Tier = 1,
        [string]$Tiers = ""
    )

    switch ($Mode) {
        "Through" { return @("--through=$Tier") }
        "Exact"   { return @("--tier=$Tier") }
        "All"     { return @("--all") }
        "Custom"  { return @("--tiers=$Tiers") }
    }
}

function Get-GpuRunLabel {
    param(
        [ValidateSet("Through", "Exact", "All", "Custom")]
        [string]$Mode,
        [int]$Tier = 1,
        [string]$Tiers = ""
    )

    switch ($Mode) {
        "Through" { return ("gpu tiers 1..{0}" -f $Tier) }
        "Exact"   { return ("gpu tier {0} only" -f $Tier) }
        "All"     { return "gpu all tiers" }
        "Custom"  { return ("gpu tiers {0}" -f $Tiers) }
    }
}

function Read-TierSetExpression {
    param([string]$Prompt = "Tier set (e.g. 1,2,4 or 1-3,5)")

    while ($true) {
        $raw = Read-Host $Prompt
        if ([string]::IsNullOrWhiteSpace($raw)) {
            return $null
        }

        $trimmed = $raw.Trim()
        if ($trimmed -eq '0') {
            return $null
        }

        $normalized = ($trimmed -replace '\s', '')
        if ($normalized -match '^(?i)(all|\d+(-\d+)?(,\d+(-\d+)?)*)$') {
            return $trimmed
        }

        Write-Host "Enter 0 (back), a comma-separated tier list (spaces OK), range, or 'all'." -ForegroundColor DarkYellow
    }
}

function Read-GpuRunSelection {
    Write-Host ""
    Write-Host " GPU run mode:" -ForegroundColor Cyan
    Write-Host "  1) Cumulative through tier N (tiers 1..N)"
    Write-Host "  2) Only tier N"
    Write-Host "  3) All tiers"
    Write-Host "  4) Custom tier set (e.g. 1,2,4 or 1-3,5)"
    Write-Host "   0) Back"

    $mode = Read-IntChoice -Prompt "Mode [0=back, 1-4] (default 1)" -Min 1 -Max 4 -Default 1 -AllowBack
    if ($null -eq $mode) {
        return $null
    }

    if ($mode -eq 3) {
        return [pscustomobject]@{
            Label     = Get-GpuRunLabel -Mode All
            Arguments = Get-GpuTierCliArguments -Mode All
        }
    }

    if ($mode -eq 4) {
        $tierSet = Read-TierSetExpression -Prompt "Tier set [0=back] (e.g. 1,2,4)"
        if ($null -eq $tierSet) {
            return $null
        }

        return [pscustomobject]@{
            Label     = Get-GpuRunLabel -Mode Custom -Tiers $tierSet
            Arguments = Get-GpuTierCliArguments -Mode Custom -Tiers $tierSet
        }
    }

    $tier = Read-IntChoice -Prompt "Tier [0=back, 1-5] (default 1)" -Min 1 -Max 5 -Default 1 -AllowBack
    if ($null -eq $tier) {
        return $null
    }

    if ($mode -eq 1) {
        return [pscustomobject]@{
            Label     = Get-GpuRunLabel -Mode Through -Tier $tier
            Arguments = Get-GpuTierCliArguments -Mode Through -Tier $tier
        }
    }

    return [pscustomobject]@{
        Label     = Get-GpuRunLabel -Mode Exact -Tier $tier
        Arguments = Get-GpuTierCliArguments -Mode Exact -Tier $tier
    }
}

function Get-GpuTestList {
    param([int]$MaxTier = 0)

    $tests = @()
    if (-not (Test-Path -LiteralPath $GpuRenderTestsExe)) {
        return $tests
    }

    Push-Location $ExeDir
    try {
        $run = Invoke-NativeCommand -ExePath $GpuRenderTestsExe -Arguments @("--all", "--list")
        foreach ($line in $run.Lines) {
            if ($line -match '^T(\d+)\s+(\S+)\s+\[([^\]]+)\]') {
                $tier = [int]$Matches[1]
                if ($MaxTier -gt 0 -and $tier -gt $MaxTier) {
                    continue
                }

                $tests += [pscustomobject]@{
                    Tier  = $tier
                    Name  = $Matches[2]
                    Label = $Matches[3]
                    Kind  = "gpu"
                }
            }
        }
    }
    finally {
        Pop-Location
    }

    return $tests
}

function Show-AvailableTests {
    Write-Host ""
    Write-Host "CPU tests:" -ForegroundColor Cyan
    $cpuTests = Get-CpuTestList
    if ($cpuTests.Count -eq 0) {
        Write-Host "  (none - build engine-tests and descriptor-heap-tests)" -ForegroundColor DarkYellow
    }
    else {
        foreach ($test in $cpuTests) {
            Write-Host ('  {0,-40} [{1}]' -f $test.Name, $test.Suite)
        }
    }

    Write-Host ""
    Write-Host "GPU tests:" -ForegroundColor Cyan
    $gpuTests = Get-GpuTestList
    if ($gpuTests.Count -eq 0) {
        Write-Host "  (none - build d3d12-render-tests)" -ForegroundColor DarkYellow
    }
    else {
        foreach ($test in $gpuTests) {
            Write-Host ('  T{0} {1,-36} [{2}]' -f $test.Tier, $test.Name, $test.Label)
        }
    }
    Write-Host ""
}

function Show-SessionSummary {
    param(
        [array]$Results,
        [TimeSpan]$TotalDuration
    )

    $ran = @($Results | Where-Object { -not $_.Skipped })
    $passed = ($ran | Measure-Object -Property Passed -Sum).Sum
    $total = ($ran | Measure-Object -Property Total -Sum).Sum
    $failedSuites = @($ran | Where-Object { $_.ExitCode -ne 0 }).Count

    Write-Host ""
    Write-Host ("{0}/{1} tests passed in {2}" -f $passed, $total, (Format-Duration $TotalDuration)) -ForegroundColor $(if ($passed -eq $total -and $failedSuites -eq 0) { "Green" } else { "Red" })

    if ($ran.Count -gt 1) {
        Write-Host ""
        Write-Host "Per suite:" -ForegroundColor DarkGray
        foreach ($result in $ran) {
            $color = if ($result.ExitCode -eq 0 -and $result.Passed -eq $result.Total) { "Green" } else { "Red" }
            Write-Host ("  {0,-28} {1}/{2} in {3}" -f $result.Label, $result.Passed, $result.Total, (Format-Duration $result.Duration)) -ForegroundColor $color
        }
    }
}

function Read-IntChoice {
    param(
        [string]$Prompt,
        [int]$Min,
        [int]$Max,
        [int]$Default,
        [switch]$AllowBack
    )

    while ($true) {
        $raw = Read-Host $Prompt
        if ([string]::IsNullOrWhiteSpace($raw)) {
            return $Default
        }

        $value = 0
        if (-not [int]::TryParse($raw, [ref]$value)) {
            if ($AllowBack) {
                Write-Host ("Enter 0 (back) or a number between {0} and {1}." -f $Min, $Max) -ForegroundColor DarkYellow
            }
            else {
                Write-Host ("Enter a number between {0} and {1}." -f $Min, $Max) -ForegroundColor DarkYellow
            }
            continue
        }

        if ($AllowBack -and $value -eq 0) {
            return $null
        }

        if ($value -ge $Min -and $value -le $Max) {
            return $value
        }

        if ($AllowBack) {
            Write-Host ("Enter 0 (back) or a number between {0} and {1}." -f $Min, $Max) -ForegroundColor DarkYellow
        }
        else {
            Write-Host ("Enter a number between {0} and {1}." -f $Min, $Max) -ForegroundColor DarkYellow
        }
    }
}

function Invoke-AllCpuTests {
    $sessionWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $results = @(
        (Invoke-TestExecutable -Label "engine-tests" -ExePath $EngineTestsExe)
        (Invoke-TestExecutable -Label "descriptor-heap-tests" -ExePath $DescriptorHeapExe)
    )
    $sessionWatch.Stop()
    Show-SessionSummary -Results $results -TotalDuration $sessionWatch.Elapsed
}

function Invoke-AllGpuTests {
    if (-not (Test-Path -LiteralPath $GpuRenderTestsExe)) {
        Write-Host "d3d12-render-tests.exe is not built." -ForegroundColor Red
        return
    }

    $selection = Read-GpuRunSelection
    if ($null -eq $selection) {
        return
    }

    $sessionWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $result = Invoke-TestExecutable -Label $selection.Label -ExePath $GpuRenderTestsExe -Arguments $selection.Arguments
    $sessionWatch.Stop()
    Show-SessionSummary -Results @($result) -TotalDuration $sessionWatch.Elapsed
}

function Invoke-Everything {
    $sessionWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $results = @(
        (Invoke-TestExecutable -Label "engine-tests" -ExePath $EngineTestsExe)
        (Invoke-TestExecutable -Label "descriptor-heap-tests" -ExePath $DescriptorHeapExe)
    )

    if (Test-Path -LiteralPath $GpuRenderTestsExe) {
        $selection = Read-GpuRunSelection
        if ($null -eq $selection) {
            return
        }

        $results += Invoke-TestExecutable -Label $selection.Label -ExePath $GpuRenderTestsExe -Arguments $selection.Arguments
    }
    else {
        Write-Host "Skipping GPU (d3d12-render-tests not built)." -ForegroundColor DarkYellow
    }

    $sessionWatch.Stop()
    Show-SessionSummary -Results $results -TotalDuration $sessionWatch.Elapsed
}

function Invoke-PickCpuTest {
    $cpuTests = Get-CpuTestList
    if ($cpuTests.Count -eq 0) {
        Write-Host "No CPU tests available." -ForegroundColor DarkYellow
        return
    }

    Write-Host ""
    for ($i = 0; $i -lt $cpuTests.Count; $i++) {
        $test = $cpuTests[$i]
        Write-Host ('  {0,2}) {1,-36} [{2}]' -f ($i + 1), $test.Name, $test.Suite)
    }
    Write-Host "   0) Back"

    $choice = Read-IntChoice -Prompt ("Pick test [0=back, 1-{0}]" -f $cpuTests.Count) -Min 1 -Max $cpuTests.Count -Default 1 -AllowBack
    if ($null -eq $choice) {
        return
    }
    $picked = $cpuTests[$choice - 1]

    $sessionWatch = [System.Diagnostics.Stopwatch]::StartNew()
    if ($picked.Suite -eq "descriptor-heap-tests") {
        $result = Invoke-TestExecutable -Label $picked.Suite -ExePath $DescriptorHeapExe
    }
    else {
        $result = Invoke-TestExecutable -Label $picked.Name -ExePath $EngineTestsExe -Arguments @("--filter=$($picked.Name)")
    }
    $sessionWatch.Stop()
    Show-SessionSummary -Results @($result) -TotalDuration $sessionWatch.Elapsed
}

function Invoke-PickGpuTest {
    if (-not (Test-Path -LiteralPath $GpuRenderTestsExe)) {
        Write-Host "d3d12-render-tests.exe is not built." -ForegroundColor Red
        return
    }

    $maxTier = Read-IntChoice -Prompt "List tests up to tier [0=back, 1-5] (default 5)" -Min 1 -Max 5 -Default 5 -AllowBack
    if ($null -eq $maxTier) {
        return
    }

    $gpuTests = Get-GpuTestList -MaxTier $maxTier
    if ($gpuTests.Count -eq 0) {
        Write-Host "No GPU tests listed." -ForegroundColor DarkYellow
        return
    }

    Write-Host ""
    for ($i = 0; $i -lt $gpuTests.Count; $i++) {
        $test = $gpuTests[$i]
        Write-Host ('  {0,2}) T{1} {2,-32} [{3}]' -f ($i + 1), $test.Tier, $test.Name, $test.Label)
    }
    Write-Host "   0) Back"

    $choice = Read-IntChoice -Prompt ("Pick test [0=back, 1-{0}]" -f $gpuTests.Count) -Min 1 -Max $gpuTests.Count -Default 1 -AllowBack
    if ($null -eq $choice) {
        return
    }
    $picked = $gpuTests[$choice - 1]

    $sessionWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $result = Invoke-TestExecutable -Label $picked.Name -ExePath $GpuRenderTestsExe -Arguments @("--filter=$($picked.Name)")
    $sessionWatch.Stop()
    Show-SessionSummary -Results @($result) -TotalDuration $sessionWatch.Elapsed
}

function Invoke-BuildTestTargets {
    $targets = @("engine-tests", "descriptor-heap-tests")
    if (Test-Path -LiteralPath (Join-Path $RepoRoot "CMakeLists.txt")) {
        $cmakeCache = Join-Path $RepoRoot (Join-Path $BuildDir "CMakeCache.txt")
        if (Test-Path -LiteralPath $cmakeCache) {
            $cacheText = Get-Content -LiteralPath $cmakeCache -Raw
            if ($cacheText -match 'GAME_ENGINE_BUILD_D3D12_RENDER_TESTS:BOOL=ON') {
                $targets += "d3d12-render-tests"
            }
        }
    }

    Write-Host ""
    Write-Host ("Building: {0}" -f ($targets -join ", ")) -ForegroundColor Cyan
    Push-Location $RepoRoot
    try {
        & cmake --build $BuildDir --config $Config --target @targets
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Build failed." -ForegroundColor Red
        }
        else {
            Write-Host "Build finished." -ForegroundColor Green
        }
    }
    finally {
        Pop-Location
    }
}

function Show-Menu {
    Write-Host ""
    Write-Host " 1) Run all CPU tests"
    Write-Host " 2) Run all GPU tests (pick tier)"
    Write-Host " 3) Run everything (CPU + GPU)"
    Write-Host " 4) Pick a CPU test"
    Write-Host " 5) Pick a GPU test"
    Write-Host " 6) List available tests"
    Write-Host " 7) Build test targets"
    Write-Host " 0) Exit"
}

Push-Location $RepoRoot
try {
    Show-Banner

    if ($Run) {
        switch ($Run) {
            "Cpu"   { Invoke-AllCpuTests; return }
            "Gpu"   {
                if (-not (Test-Path -LiteralPath $GpuRenderTestsExe)) {
                    Write-Host "d3d12-render-tests.exe is not built." -ForegroundColor Red
                    exit 1
                }

                $gpuMode = if ([string]::IsNullOrWhiteSpace($GpuMode)) { "Through" } else { $GpuMode }
                if ($gpuMode -eq "Custom" -and [string]::IsNullOrWhiteSpace($Tiers)) {
                    Write-Host "GpuMode Custom requires -Tiers (e.g. -Tiers '1,2,4')." -ForegroundColor Red
                    exit 1
                }

                $sessionWatch = [System.Diagnostics.Stopwatch]::StartNew()
                $result = Invoke-TestExecutable `
                    -Label (Get-GpuRunLabel -Mode $gpuMode -Tier $Tier -Tiers $Tiers) `
                    -ExePath $GpuRenderTestsExe `
                    -Arguments (Get-GpuTierCliArguments -Mode $gpuMode -Tier $Tier -Tiers $Tiers)
                $sessionWatch.Stop()
                Show-SessionSummary -Results @($result) -TotalDuration $sessionWatch.Elapsed
                if ($result.ExitCode -ne 0 -or $result.Passed -ne $result.Total) { exit 1 }
                return
            }
            "All"   { Invoke-Everything; return }
            "List"  { Show-AvailableTests; return }
            "Build" { Invoke-BuildTestTargets; return }
        }
    }

    while ($true) {
        Show-Menu
        $choice = Read-IntChoice -Prompt "Choice" -Min 0 -Max 7 -Default 1

        switch ($choice) {
            0 { return }
            1 { Invoke-AllCpuTests }
            2 { Invoke-AllGpuTests }
            3 { Invoke-Everything }
            4 { Invoke-PickCpuTest }
            5 { Invoke-PickGpuTest }
            6 { Show-AvailableTests }
            7 { Invoke-BuildTestTargets }
        }
    }
}
finally {
    Pop-Location
}
