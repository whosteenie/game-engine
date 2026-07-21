[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SingleLog,

    [Parameter(Mandatory = $true)]
    [string]$DualLog
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-CadenceRecords([string]$Path) {
    $records = @()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\[frame\] dlss-trace app_serial=(\d+) order=(\d+) viewport=(\d+).* outcome=([^ ]+) reason=([^ ]+).* token=([^ ]+)$') {
            $records += [pscustomobject]@{
                App = [uint64]$Matches[1]
                Order = [int]$Matches[2]
                Viewport = [int]$Matches[3]
                Outcome = $Matches[4]
                Reason = $Matches[5]
                Token = if ($Matches[6] -eq 'none') { $null } else { [uint64]$Matches[6] }
            }
        }
    }
    return $records
}

function Test-Cadence([string]$Path, [bool]$RequireDual) {
    $records = @(Read-CadenceRecords $Path)
    $tokenRecords = @($records | Where-Object { $null -ne $_.Token })
    if ($tokenRecords.Count -eq 0) {
        throw "No token-bearing evaluations in $Path"
    }

    $frames = @($tokenRecords | Group-Object App | Sort-Object { [uint64]$_.Name })
    $sharedTokenViolations = @($frames | Where-Object {
        @($_.Group.Token | Sort-Object -Unique).Count -ne 1
    })
    if ($sharedTokenViolations.Count -ne 0) {
        throw "$Path has $($sharedTokenViolations.Count) application frames with multiple tokens"
    }

    $cadenceViolations = 0
    for ($i = 1; $i -lt $frames.Count; ++$i) {
        $previous = $frames[$i - 1].Group[0]
        $current = $frames[$i].Group[0]
        if (($current.Token - $previous.Token) -ne ($current.App - $previous.App)) {
            ++$cadenceViolations
        }
    }
    if ($cadenceViolations -ne 0) {
        throw "$Path has $cadenceViolations non-unit application-frame cadence transitions"
    }

    $dualFrames = @($frames | Where-Object {
        @($_.Group.Viewport | Sort-Object -Unique).Count -ge 2
    })
    $skipped = @($records | Where-Object { $_.Outcome -eq 'skipped' })
    $failed = @($records | Where-Object { $_.Outcome -eq 'failed' })
    if ($failed.Count -ne 0) {
        throw "$Path contains $($failed.Count) failed SDK evaluations"
    }
    if ($RequireDual -and $dualFrames.Count -eq 0) {
        throw "$Path contains no frame evaluated by both viewport IDs"
    }
    if (-not $RequireDual -and $skipped.Count -eq 0) {
        throw "$Path contains no skipped-viewport evidence"
    }

    return [ordered]@{
        path = (Resolve-Path -LiteralPath $Path).Path
        records = $records.Count
        token_records = $tokenRecords.Count
        token_frames = $frames.Count
        first_application_frame = $frames[0].Group[0].App
        first_token = $frames[0].Group[0].Token
        last_application_frame = $frames[-1].Group[0].App
        last_token = $frames[-1].Group[0].Token
        shared_token_violations = $sharedTokenViolations.Count
        cadence_violations = $cadenceViolations
        frames_with_both_viewports = $dualFrames.Count
        skipped_records = $skipped.Count
        failed_records = $failed.Count
        viewport_ids = @($tokenRecords.Viewport | Sort-Object -Unique)
    }
}

$single = Test-Cadence $SingleLog $false
$dual = Test-Cadence $DualLog $true
$diagnosticMatches = @(Select-String -LiteralPath @($SingleLog, $DualLog) `
    -Pattern 'streamline.*(error|warn)|D3D12.*(error|warning)|LIVE OBJECT|device removed|outcome=failed|application-frame-token-unavailable' `
    -CaseSensitive:$false)
if ($diagnosticMatches.Count -ne 0) {
    throw "SDK/D3D12 diagnostic scan found $($diagnosticMatches.Count) error or warning records"
}

[ordered]@{
    pass = 'S1-P3'
    single = $single
    dual = $dual
    sdk_d3d12_diagnostic_matches = $diagnosticMatches.Count
    result = 'PASS'
} | ConvertTo-Json -Depth 5
