param(
    [Parameter(Mandatory = $true)]
    [string]$Log
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Log)) {
    throw "History trace log not found: $Log"
}

$historyLines = @(
    Select-String -LiteralPath $Log -Pattern '^\[frame\] history-trace ' |
        ForEach-Object { $_.Line }
)
$restirLines = @($historyLines | Where-Object { $_ -match ' owner=restir-temporal ' })
if ($restirLines.Count -eq 0) {
    throw 'No ReSTIR temporal history records were captured.'
}

$records = @(
    foreach ($line in $restirLines) {
        if ($line -notmatch 'app_serial=(\d+) viewport=(\d+) owner=restir-temporal event=(request|consume) generation=(\d+).* render=([^ ]+)') {
            throw "Malformed ReSTIR history record: $line"
        }
        [pscustomobject]@{
            ApplicationSerial = [uint64]$Matches[1]
            Viewport = [uint32]$Matches[2]
            Event = $Matches[3]
            Generation = [uint32]$Matches[4]
            RenderExtent = $Matches[5]
        }
    }
)

$unexpectedViewports = @($records | Where-Object { $_.Viewport -notin 0, 1 })
if ($unexpectedViewports.Count -ne 0) {
    throw "Unexpected viewport ids in ReSTIR history: $((@($unexpectedViewports.Viewport | Sort-Object -Unique)) -join ', ')"
}

$summaries = @()
foreach ($viewport in 0, 1) {
    $viewRecords = @($records | Where-Object { $_.Viewport -eq $viewport })
    $requests = @($viewRecords | Where-Object { $_.Event -eq 'request' })
    $consumes = @($viewRecords | Where-Object { $_.Event -eq 'consume' })
    if ($requests.Count -eq 0 -or $consumes.Count -eq 0) {
        throw "Viewport $viewport did not record both reset requests and compatible consumes."
    }
    if ($viewRecords[0].Event -ne 'request' -or $viewRecords[0].Generation -ne 1) {
        throw "Viewport $viewport did not start with its own generation-1 request."
    }

    $firstConsumeSerial = ($consumes | Measure-Object -Property ApplicationSerial -Minimum).Minimum
    $lateRequests = @($requests | Where-Object {
        $_.ApplicationSerial -gt $firstConsumeSerial
    })
    if ($lateRequests.Count -ne 0) {
        throw "Viewport $viewport requested another reset after entering compatible steady state."
    }

    $summaries += [pscustomobject]@{
        Viewport = $viewport
        Total = $viewRecords.Count
        Requests = $requests.Count
        Consumes = $consumes.Count
        Extents = (@($viewRecords.RenderExtent | Sort-Object -Unique) -join ',')
        FinalGeneration = ($viewRecords | Measure-Object -Property Generation -Maximum).Maximum
    }
}

$gpuErrors = @(
    Select-String -LiteralPath $Log -Pattern '\[error\]|device removed|D3D12 ERROR|DXGI_ERROR|GPU validation error' -CaseSensitive:$false
)
if ($gpuErrors.Count -ne 0) {
    throw "GPU/D3D12 error records found: $($gpuErrors.Count)"
}

foreach ($summary in $summaries) {
    Write-Output (
        "viewport={0} total={1} request={2} consume={3} extents={4} final_generation={5}" -f
        $summary.Viewport,
        $summary.Total,
        $summary.Requests,
        $summary.Consumes,
        $summary.Extents,
        $summary.FinalGeneration
    )
}
Write-Output "gpu_error_matches=$($gpuErrors.Count)"
Write-Output 'PASS: Scene and Game retain independent ReSTIR history sequences.'
