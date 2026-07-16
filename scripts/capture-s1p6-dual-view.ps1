# Captures Scene+Game only after both Streamline viewport IDs evaluate in shared application frames.
[CmdletBinding()]
param(
    [string]$Project = 'test_proj/DEMO-SCENE/DEMO-SCENE.gameproject',
    [string]$BuildDir = 'build',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputDirectory = 'diagnostics/s1p6/dual-view-final',
    [ValidateRange(20, 120)][int]$TimeoutSeconds = 45
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectPath = (Resolve-Path (Join-Path $repoRoot $Project)).Path
$enginePath = Join-Path $repoRoot "$BuildDir\$Config\game-engine.exe"
if (!(Test-Path -LiteralPath $enginePath)) { throw "Missing $enginePath; build game-engine first." }
$outputRoot = Join-Path $repoRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$stdout = Join-Path $outputRoot 'stdout.log'
$stderr = Join-Path $outputRoot 'stderr.log'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class S1P6WindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
'@

function Get-WindowCaptureInfo([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    $handle = $Process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $null }
    $rect = New-Object S1P6WindowCapture+Rect
    if (![S1P6WindowCapture]::GetWindowRect($handle, [ref]$rect)) { return $null }
    if ($rect.R -le $rect.L -or $rect.B -le $rect.T) { return $null }
    return [pscustomobject]@{ Handle=$handle; Rect=$rect; Width=$rect.R-$rect.L; Height=$rect.B-$rect.T }
}

function Save-PrintWindow($Info, [string]$Path) {
    $bitmap = New-Object System.Drawing.Bitmap $Info.Width, $Info.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try { $ok = [S1P6WindowCapture]::PrintWindow($Info.Handle, $hdc, 2) }
        finally { $graphics.ReleaseHdc($hdc) }
        if ($ok) { $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png) }
        return $ok
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}

function Save-ScreenWindow($Info, [string]$Path) {
    $bitmap = New-Object System.Drawing.Bitmap $Info.Width, $Info.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($Info.Rect.L, $Info.Rect.T, 0, 0, $bitmap.Size)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}

$names = @('GAME_ENGINE_AUTO_OPEN','GAME_ENGINE_FRAME_DEBUG','GAME_ENGINE_LOG',
    'GAME_ENGINE_AUTOMATION_DUAL_VIEW','GAME_ENGINE_S1P4_DUAL_OWNERSHIP',
    'GAME_ENGINE_S1P4_TRANSITIONS','GAME_ENGINE_S0P3_TRANSITIONS')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:GAME_ENGINE_AUTO_OPEN = $projectPath
    $env:GAME_ENGINE_FRAME_DEBUG = '1'
    $env:GAME_ENGINE_LOG = '1'
    $env:GAME_ENGINE_AUTOMATION_DUAL_VIEW = '1'
    foreach ($unset in @('GAME_ENGINE_S1P4_DUAL_OWNERSHIP','GAME_ENGINE_S1P4_TRANSITIONS','GAME_ENGINE_S0P3_TRANSITIONS')) {
        Remove-Item "Env:$unset" -ErrorAction SilentlyContinue
    }
    $process = Start-Process -FilePath $enginePath -WorkingDirectory (Split-Path $enginePath) `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $dualFrames = @()
    while ([DateTime]::UtcNow -lt $deadline -and !$process.HasExited) {
        if (Test-Path -LiteralPath $stderr) {
            $records = @(Select-String -LiteralPath $stderr -Pattern '^\[frame\] dlss-trace ' | ForEach-Object Line)
            $parsed = foreach ($line in $records) {
                if ($line -match 'app_serial=(\d+).* viewport=(\d+).* outcome=evaluated') {
                    [pscustomobject]@{ App=[uint64]$Matches[1]; Viewport=[int]$Matches[2] }
                }
            }
            $dualFrames = @($parsed | Group-Object App | Where-Object {
                @($_.Group.Viewport | Sort-Object -Unique).Count -ge 2
            })
            if ($dualFrames.Count -ge 3) { break }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($dualFrames.Count -lt 3) { throw 'No stable Scene+Game shared-evaluation window was observed.' }
    Start-Sleep -Seconds 2
    $info = Get-WindowCaptureInfo $process
    if ($null -eq $info) { throw 'Could not resolve the engine window for Scene+Game capture.' }
    $printPath = Join-Path $outputRoot 'scene-game-printwindow.png'
    $screenPath = Join-Path $outputRoot 'scene-game-screen.png'
    $printOk = Save-PrintWindow $info $printPath
    Save-ScreenWindow $info $screenPath
    if (!$process.HasExited) { Stop-Process -Id $process.Id; $process.WaitForExit() }
    $diagnostics = @(Select-String -LiteralPath $stderr -Pattern (
        'streamline.*(error|warn)|D3D12.*(error|warning)|LIVE OBJECT|device removed|DXGI_ERROR|outcome=failed') `
        -CaseSensitive:$false)
    if ($diagnostics.Count -ne 0) { throw "Scene+Game capture has $($diagnostics.Count) SDK/D3D matches." }
    [ordered]@{
        record_type = 's1p6_scene_game_capture'
        schema_version = 1
        project = $projectPath
        shared_evaluation_frames = $dualFrames.Count
        viewport_ids = @(0,1)
        print_window_succeeded = $printOk
        print_window = if ($printOk) { $printPath } else { $null }
        screen_window = $screenPath
        sdk_d3d12_diagnostic_matches = 0
        stdout = $stdout
        stderr = $stderr
        result = 'PASS'
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputRoot 'capture.json') -Encoding utf8
    Write-Host "S1-P6 Scene+Game capture PASS: $outputRoot" -ForegroundColor Green
}
finally {
    foreach ($name in $names) {
        if ($null -eq $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
}
