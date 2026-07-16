# Captures and validates the S1-P4 per-viewport compatibility/reset transition matrix.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [ValidateSet('FullMatrix', 'DualOwnership')][string]$Mode = 'FullMatrix',
    [ValidateRange(20, 180)][int]$DurationSeconds = 45,
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptRoot = if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    (Get-Location).Path
} else {
    $PSScriptRoot
}
$repoRoot = if (Test-Path -LiteralPath (Join-Path $scriptRoot 'CMakeLists.txt')) {
    (Resolve-Path $scriptRoot).Path
} else {
    (Resolve-Path (Join-Path $scriptRoot '..')).Path
}
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot (
        'diagnostics\s1p4-history-compatibility\demo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class S1P4WindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
'@
function Save-WindowCapture([System.Diagnostics.Process]$Process, [string]$Path) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($null -eq $handle -or [string]::IsNullOrWhiteSpace([string]$handle) `
        -or $handle -eq [IntPtr]::Zero) { return $false }
    $rect = New-Object S1P4WindowCapture+Rect
    if (![S1P4WindowCapture]::GetWindowRect($handle, [ref]$rect)) { return $false }
    $width = $rect.R - $rect.L
    $height = $rect.B - $rect.T
    if ($width -le 0 -or $height -le 0) { return $false }
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try {
            if (![S1P4WindowCapture]::PrintWindow($handle, $hdc, 2)) { return $false }
        } finally {
            $graphics.ReleaseHdc($hdc)
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    return $true
}

$saved = @{}
$names = @(
    'GAME_ENGINE_AUTO_OPEN',
    'GAME_ENGINE_FRAME_DEBUG',
    'GAME_ENGINE_LOG',
    'GAME_ENGINE_AUTOMATION_DUAL_VIEW',
    'GAME_ENGINE_S1P4_DUAL_OWNERSHIP',
    'GAME_ENGINE_S1P4_TRANSITIONS')
foreach ($name in $names) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'
    if ($Mode -eq 'DualOwnership') {
        $env:GAME_ENGINE_AUTOMATION_DUAL_VIEW = '1'
        $env:GAME_ENGINE_S1P4_DUAL_OWNERSHIP = '1'
        Remove-Item Env:GAME_ENGINE_S1P4_TRANSITIONS -ErrorAction SilentlyContinue
    } else {
        Remove-Item Env:GAME_ENGINE_AUTOMATION_DUAL_VIEW -ErrorAction SilentlyContinue
        Remove-Item Env:GAME_ENGINE_S1P4_DUAL_OWNERSHIP -ErrorAction SilentlyContinue
        $env:GAME_ENGINE_S1P4_TRANSITIONS = '1'
    }
    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru

    $firstCaptureDelay = [Math]::Max(8, [Math]::Floor($DurationSeconds / 3))
    Start-Sleep -Seconds $firstCaptureDelay
    $captureA = Join-Path $OutputDirectory 'transition-a.png'
    $capturedA = Save-WindowCapture $process $captureA
    Start-Sleep -Seconds $firstCaptureDelay
    $captureB = Join-Path $OutputDirectory 'transition-b.png'
    $capturedB = Save-WindowCapture $process $captureB
    $remaining = $DurationSeconds - (2 * $firstCaptureDelay)
    if ($remaining -gt 0) { Start-Sleep -Seconds $remaining }
    if (!$process.HasExited) { Stop-Process -Id $process.Id; $process.WaitForExit() }

    $keyLines = @(Select-String -LiteralPath $stderr -Pattern '^\[frame\] history-key ' |
        ForEach-Object Line)
    $historyLines = @(Select-String -LiteralPath $stderr -Pattern '^\[frame\] history-trace ' |
        ForEach-Object Line)
    if ($keyLines.Count -eq 0) { throw 'No S1-P4 history-key records were captured.' }

    $records = foreach ($line in $keyLines) {
        if ($line -notmatch 'app_serial=(\d+) viewport=(\d+) event=([^ ]+)') { continue }
        $serial = [uint64]$Matches[1]
        $viewport = [uint32]$Matches[2]
        $eventName = $Matches[3]
        $feature = if ($line -match ' feature=([^ ]+)') { $Matches[1] } else { '' }
        if ($line -notmatch 'reason_bits=0x([0-9A-Fa-f]+) owner_bits=0x([0-9A-Fa-f]+)') {
            continue
        }
        [pscustomobject]@{
            Serial = $serial
            Viewport = $viewport
            Event = $eventName
            Feature = $feature
            Reasons = [Convert]::ToUInt32($Matches[1], 16)
            Owners = [Convert]::ToUInt32($Matches[2], 16)
            Line = $line
        }
    }
    $schedules = @($records | Where-Object Event -eq 'schedule')
    $compatibles = @($records | Where-Object Event -eq 'compatible')
    $commits = @($records | Where-Object Event -eq 'commit')
    if ($schedules.Count -eq 0 -or $compatibles.Count -eq 0 -or $commits.Count -eq 0) {
        throw 'Capture is missing schedule, compatible, or commit records.'
    }

    $requiredReasons = if ($Mode -eq 'FullMatrix') { [ordered]@{
        first_frame = 0x001
        producer = 0x002
        guide = 0x004
        feature = 0x008
        quality = 0x010
        extent = 0x060
        camera_invalid = 0x080
        camera_cut = 0x100
        diagnostic = 0x200
    } } else { [ordered]@{ first_frame = 0x001 } }
    $missingReasons = @()
    foreach ($entry in $requiredReasons.GetEnumerator()) {
        if (@($schedules | Where-Object { ($_.Reasons -band $entry.Value) -ne 0 }).Count -eq 0) {
            $missingReasons += $entry.Key
        }
    }

    $policyFailures = @()
    $committedFeature = @{}
    foreach ($record in $records) {
        $viewportKey = [string]$record.Viewport
        if ($record.Event -eq 'schedule') {
            $allOwnerReasons = 0x001 -bor 0x002 -bor 0x020 -bor 0x080 -bor 0x100
            $expectedOwners = if (($record.Reasons -band $allOwnerReasons) -ne 0) { 0x1F } else { 0x05 }
            if (($record.Reasons -band 0x008) -ne 0 -and $committedFeature.ContainsKey($viewportKey)) {
                $previousUsesDisplay = $committedFeature[$viewportKey] -in @('dlss', 'rr')
                $currentUsesDisplay = $record.Feature -in @('dlss', 'rr')
                if (!$previousUsesDisplay -or !$currentUsesDisplay) {
                    $expectedOwners = $expectedOwners -bor 0x02
                }
            }
            if ($record.Owners -ne $expectedOwners) { $policyFailures += $record.Line }
        } elseif ($record.Event -eq 'compatible') {
            if ($record.Reasons -ne 0 -or $record.Owners -ne 0) {
                $policyFailures += $record.Line
            }
        } elseif ($record.Event -eq 'commit') {
            $committedFeature[$viewportKey] = $record.Feature
        }
    }

    $duplicateSchedules = @($schedules | Group-Object Serial, Viewport |
        Where-Object Count -ne 1)
    $missingCommits = @()
    foreach ($begin in @($schedules) + @($compatibles)) {
        if (@($commits | Where-Object {
            $_.Serial -eq $begin.Serial -and $_.Viewport -eq $begin.Viewport
        }).Count -ne 1) {
            $missingCommits += "$($begin.Serial):$($begin.Viewport)"
        }
    }
    $viewports = @($records | Select-Object -ExpandProperty Viewport -Unique | Sort-Object)
    $runtimeFailures = @(Select-String -LiteralPath $stderr -Pattern (
        'D3D12 device was removed|\[error\] \[dlss\]|evaluate-failed') | ForEach-Object Line)
    $captureFiles = @()
    if ($capturedA) { $captureFiles += $captureA }
    if ($capturedB) { $captureFiles += $captureB }

    $result = [ordered]@{
        project = $projectPath
        mode = $Mode
        duration_seconds = $DurationSeconds
        key_records = $keyLines.Count
        history_records = $historyLines.Count
        schedule_records = $schedules.Count
        compatible_records = $compatibles.Count
        commit_records = $commits.Count
        viewports = $viewports
        missing_reasons = $missingReasons
        policy_failures = $policyFailures.Count
        duplicate_schedule_frames = $duplicateSchedules.Count
        missing_or_duplicate_commits = $missingCommits
        screenshots = $captureFiles
        runtime_failures = $runtimeFailures
        stdout = $stdout
        stderr = $stderr
        transition_frames = if ($Mode -eq 'FullMatrix') {
            '10 raster; 20 hybrid; 30 PT; 40 DLSS; 50 RR; 60 DLAA; 70 SR performance/extent; 80/90 guide diagnostic; 110 camera cut; 120 invalid camera; 130 hybrid; 140 PT'
        } else { 'raster/no-AA steady dual ownership' }
    }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
        Join-Path $OutputDirectory 'capture.json') -Encoding utf8
    Write-Host (
        "S1-P4 capture: $OutputDirectory; key=$($keyLines.Count) schedule=$($schedules.Count) " +
        "compatible=$($compatibles.Count) commit=$($commits.Count) viewports=$($viewports -join ',')")
    if ($missingReasons.Count -ne 0 -or $policyFailures.Count -ne 0 `
        -or $duplicateSchedules.Count -ne 0 -or $missingCommits.Count -ne 0 `
        -or $runtimeFailures.Count -ne 0 `
        -or ($Mode -eq 'DualOwnership' -and $viewports.Count -lt 2) `
        -or ($Mode -eq 'FullMatrix' -and $viewports.Count -lt 1) `
        -or $captureFiles.Count -lt 1) {
        exit 2
    }
}
finally {
    foreach ($name in $names) {
        if ($null -eq $saved[$name]) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
        }
    }
}
