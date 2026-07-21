[CmdletBinding()]
param(
    [ValidateSet("All", "Capture", "Configure", "Build", "Incremental", "Deployment", "Nrd", "Invalidation", "Inventory")]
    [string]$Stage = "All",
    [string]$Label = "baseline-$(Get-Date -Format 'yyyyMMdd-HHmmss')",
    [string]$DependencyCache = "build/_deps",
    [int]$CleanSamples = 3,
    [int]$IncrementalSamples = 5
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$DependencyCache = (Resolve-Path (Join-Path $Root $DependencyCache)).Path
$Evidence = Join-Path $Root "artifacts/build-bench/$Label"
$Trees = Join-Path $Root "artifacts/build-bench/trees/$Label"
$PrimaryTree = Join-Path $Trees "clean-01"
$SourceCacheFile = Join-Path $Evidence "dependency-source-cache.txt"
New-Item -ItemType Directory -Force -Path $Evidence, $Trees | Out-Null

function Write-TextFile([string]$Path, [object[]]$Value) {
    $Value | Out-File -LiteralPath $Path -Encoding utf8
}

function Invoke-Recorded {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $log = Join-Path $Evidence "$Name.log"
    # Tee-Object in Windows PowerShell 5 appends UTF-16. Start the file in the same encoding so
    # warnings and compiler failures remain searchable instead of producing a mixed-encoding log.
    "command=$Command $($Arguments -join ' ')" | Out-File -LiteralPath $log -Encoding unicode
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5 wraps native stderr as ErrorRecord objects. CMake emits warnings on
        # stderr, so native exit status—not stream choice—must determine benchmark success.
        $ErrorActionPreference = "Continue"
        & $Command @Arguments 2>&1 | Tee-Object -FilePath $log -Append
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    $watch.Stop()
    "exit=$exitCode elapsed_seconds=$([math]::Round($watch.Elapsed.TotalSeconds, 3))" |
        Tee-Object -FilePath $log -Append
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "$Name failed with exit code $exitCode (see $log)"
    }
    return $exitCode
}

function Get-DependencyArguments {
    $names = @(
        "glfw", "glm", "imgui", "tinygltf", "json", "nrd", "streamline",
        "joltphysics", "d3d12memoryallocator", "mikktspace", "shadermake", "mathlib", "dxc"
    )
    $args = @()
    $records = @()
    foreach ($name in $names) {
        $source = Join-Path $DependencyCache "$name-src"
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Missing dependency source cache: $source"
        }
        $cacheName = $name.ToUpperInvariant()
        $args += "-DFETCHCONTENT_SOURCE_DIR_${cacheName}:PATH=$source"
        $records += "$cacheName=$source"
    }
    Write-TextFile $SourceCacheFile $records
    return $args
}

function Get-ConfigureArguments([string]$Tree, [bool]$EmbedDxil = $true) {
    $shaderOutput = Join-Path $Tree "nrd-shaders"
    $embed = if ($EmbedDxil) { "ON" } else { "OFF" }
    return @(
        "-S", $Root, "-B", $Tree, "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DGAME_ENGINE_ENABLE_DLSS=ON",
        "-DGAME_ENGINE_D3D12_DEBUG_LAYER=ON",
        "-DGAME_ENGINE_BUILD_D3D12_RENDER_TESTS=ON",
        "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
        "-DNRD_SHADERS_PATH:PATH=$shaderOutput",
        "-DNRD_EMBEDS_DXIL_SHADERS:BOOL=$embed"
    ) + (Get-DependencyArguments)
}

function Invoke-Build([string]$Name, [string]$Tree, [string]$Target, [switch]$AllowFailure) {
    $args = @("--build", $Tree, "--config", "Debug", "--target", $Target, "--", "/m", "/v:minimal")
    return Invoke-Recorded -Name $Name -Command "cmake" -Arguments $args -AllowFailure:$AllowFailure
}

function Invoke-TimestampProbe([string]$Prefix, [string]$RelativePath) {
    $path = Join-Path $Root $RelativePath
    $originalTime = (Get-Item -LiteralPath $path).LastWriteTimeUtc
    $originalHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    try {
        for ($sample = 1; $sample -le $IncrementalSamples; $sample++) {
            (Get-Item -LiteralPath $path).LastWriteTimeUtc = [DateTime]::UtcNow
            Invoke-Build -Name ("{0}-{1:D2}" -f $Prefix, $sample) -Tree $PrimaryTree -Target "game-engine"
            $afterHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($afterHash -ne $originalHash) { throw "Timestamp probe changed content: $path" }
        }
    }
    finally {
        (Get-Item -LiteralPath $path).LastWriteTimeUtc = $originalTime
    }
    Write-TextFile (Join-Path $Evidence "$Prefix-input.txt") @(
        "path=$RelativePath", "sha256=$originalHash", "samples=$IncrementalSamples"
    )
}

function Capture-Environment {
    Write-TextFile (Join-Path $Evidence "commands.txt") @(
        "stage=$Stage", "label=$Label", "clean_samples=$CleanSamples", "incremental_samples=$IncrementalSamples",
        "generator=Visual Studio 17 2022", "platform=x64", "configuration=Debug", "target=game-engine",
        "GAME_ENGINE_ENABLE_DLSS=ON", "GAME_ENGINE_D3D12_DEBUG_LAYER=ON",
        "GAME_ENGINE_BUILD_D3D12_RENDER_TESTS=ON", "FETCHCONTENT_UPDATES_DISCONNECTED=ON"
    )
    & git -c "safe.directory=$($Root.Replace('\', '/'))" rev-parse HEAD |
        Out-File -LiteralPath (Join-Path $Evidence "revision.txt") -Encoding utf8
    & git -c "safe.directory=$($Root.Replace('\', '/'))" status --short |
        Out-File -LiteralPath (Join-Path $Evidence "worktree.txt") -Encoding utf8
    & cmake --version | Out-File -LiteralPath (Join-Path $Evidence "cmake-version.txt") -Encoding utf8
    $ninjaVersion = try { & ninja --version 2>&1 } catch { "unavailable: $($_.Exception.Message)" }
    $ninjaVersion | Out-File -LiteralPath (Join-Path $Evidence "ninja-version.txt") -Encoding utf8
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        & $vswhere -latest -products * -format json |
            Out-File -LiteralPath (Join-Path $Evidence "visual-studio.json") -Encoding utf8
    } else {
        Write-TextFile (Join-Path $Evidence "visual-studio.json") @("unavailable: $vswhere")
    }
    foreach ($item in @(@("cpu", "Win32_Processor"), @("system", "Win32_ComputerSystem"), @("os", "Win32_OperatingSystem"))) {
        try {
            Get-CimInstance $item[1] | Format-List * | Out-String |
                Out-File -LiteralPath (Join-Path $Evidence "$($item[0]).txt") -Encoding utf8
        } catch {
            Write-TextFile (Join-Path $Evidence "$($item[0]).txt") @("unavailable: $($_.Exception.Message)")
        }
    }
    try {
        Get-Volume -DriveLetter C | Format-List * | Out-String |
            Out-File -LiteralPath (Join-Path $Evidence "storage.txt") -Encoding utf8
    } catch {
        Write-TextFile (Join-Path $Evidence "storage.txt") @("unavailable: $($_.Exception.Message)")
    }
}

function Configure-Baseline {
    for ($sample = 1; $sample -le $CleanSamples; $sample++) {
        $tree = Join-Path $Trees ("clean-{0:D2}" -f $sample)
        if (Test-Path -LiteralPath (Join-Path $tree "CMakeCache.txt")) {
            throw "Refusing to reuse configured clean-sample tree: $tree"
        }
        New-Item -ItemType Directory -Force -Path $tree | Out-Null
        Invoke-Recorded -Name ("configure-{0:D2}" -f $sample) -Command "cmake" -Arguments (Get-ConfigureArguments $tree)
        Copy-Item -LiteralPath (Join-Path $tree "CMakeCache.txt") -Destination (Join-Path $Evidence ("cache-{0:D2}.txt" -f $sample))
    }
}

function Build-Baseline {
    for ($sample = 1; $sample -le $CleanSamples; $sample++) {
        $tree = Join-Path $Trees ("clean-{0:D2}" -f $sample)
        Invoke-Build -Name ("clean-build-{0:D2}" -f $sample) -Tree $tree -Target "game-engine"
        Get-ChildItem (Join-Path $tree "Debug") -File -ErrorAction SilentlyContinue |
            Select-Object Name, Length, LastWriteTimeUtc |
            Format-Table -AutoSize | Out-String |
            Out-File -LiteralPath (Join-Path $Evidence ("outputs-{0:D2}.txt" -f $sample)) -Encoding utf8
    }
    for ($sample = 1; $sample -le $IncrementalSamples; $sample++) {
        Invoke-Build -Name ("noop-{0:D2}" -f $sample) -Tree $PrimaryTree -Target "game-engine"
    }
}

function Run-Incremental {
    Invoke-TimestampProbe "cpp-edit-app" "src/app/editor/TuningSectionState.cpp"
    Invoke-TimestampProbe "cpp-edit-engine" "src/engine/scene/RotationUtils.cpp"
    Invoke-TimestampProbe "header-edit" "src/engine/rhi/GfxContext.h"
    Invoke-Build -Name "explicit-engine-tests" -Tree $PrimaryTree -Target "engine-tests"
    Invoke-Build -Name "all-build" -Tree $PrimaryTree -Target "ALL_BUILD"
}

function Run-DeploymentInvalidation {
    $assetRelative = "shaders/selection_mask.ps.hlsl"
    $assetSource = Join-Path $Root "assets/$assetRelative"
    $assetDestination = Join-Path $PrimaryTree "Debug/assets/$assetRelative"
    # game_engine_copy_dxc_runtime prefers the project-vendored redist over ShaderMake's fetched DXC.
    $runtimeSource = Join-Path $Root "vendor/dxc/x64/dxil.dll"
    $runtimeDestination = Join-Path $PrimaryTree "Debug/dxil.dll"
    $records = @()

    Invoke-Build -Name "deployment-noop" -Tree $PrimaryTree -Target "game-engine"
    foreach ($probe in @(@("asset", $assetSource, $assetDestination), @("runtime", $runtimeSource, $runtimeDestination))) {
        $kind, $source, $destination = $probe
        if (-not (Test-Path -LiteralPath $destination)) { throw "Missing deployment probe destination: $destination" }
        $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        Remove-Item -LiteralPath $destination
        Invoke-Build -Name "deployment-missing-$kind" -Tree $PrimaryTree -Target "game-engine" -AllowFailure | Out-Null
        $restored = Test-Path -LiteralPath $destination
        $restoredHash = if ($restored) { (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash } else { "missing" }
        $records += "$kind restored=$restored source_sha256=$sourceHash destination_sha256=$restoredHash"
        if (-not $restored) { Copy-Item -LiteralPath $source -Destination $destination -Force }
    }

    $assetTime = (Get-Item -LiteralPath $assetSource).LastWriteTimeUtc
    try {
        (Get-Item -LiteralPath $assetSource).LastWriteTimeUtc = [DateTime]::UtcNow
        Invoke-Build -Name "deployment-changed-asset" -Tree $PrimaryTree -Target "game-engine" -AllowFailure | Out-Null
        $records += "changed_asset source_sha256=$((Get-FileHash -LiteralPath $assetSource -Algorithm SHA256).Hash) destination_sha256=$((Get-FileHash -LiteralPath $assetDestination -Algorithm SHA256).Hash)"
    } finally {
        (Get-Item -LiteralPath $assetSource).LastWriteTimeUtc = $assetTime
    }
    Write-TextFile (Join-Path $Evidence "deployment-results.txt") $records
}

function Run-NrdInvalidation {
    $nrdSourceRoot = Join-Path $DependencyCache "nrd-src"
    $input = Join-Path $nrdSourceRoot "Shaders/Clear.cs.hlsl"
    $output = Join-Path $PrimaryTree "nrd-shaders/Clear.cs.dxil.h"
    $inputTime = (Get-Item -LiteralPath $input).LastWriteTimeUtc
    $inputHash = (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash
    try {
        (Get-Item -LiteralPath $input).LastWriteTimeUtc = [DateTime]::UtcNow
        Invoke-Build -Name "nrd-touched-shader" -Tree $PrimaryTree -Target "game-engine"
    } finally {
        (Get-Item -LiteralPath $input).LastWriteTimeUtc = $inputTime
    }
    $touchOutputHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash

    Remove-Item -LiteralPath $output
    Invoke-Build -Name "nrd-missing-output" -Tree $PrimaryTree -Target "game-engine"
    $missingRestored = Test-Path -LiteralPath $output
    $restoredHash = if ($missingRestored) { (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash } else { "missing" }

    $optionTree = Join-Path $Trees "nrd-option"
    if (Test-Path -LiteralPath (Join-Path $optionTree "CMakeCache.txt")) {
        throw "Refusing to reuse NRD option tree: $optionTree"
    }
    Invoke-Recorded -Name "nrd-option-configure-off" -Command "cmake" -Arguments (Get-ConfigureArguments $optionTree $false)
    Invoke-Build -Name "nrd-option-build-off" -Tree $optionTree -Target "NRD"
    Invoke-Recorded -Name "nrd-option-configure-on" -Command "cmake" -Arguments (Get-ConfigureArguments $optionTree $true)
    Invoke-Build -Name "nrd-option-build-on" -Tree $optionTree -Target "NRD"

    Write-TextFile (Join-Path $Evidence "nrd-results.txt") @(
        "input=$input", "input_sha256=$inputHash", "output=$output",
        "touch_output_sha256=$touchOutputHash", "missing_output_restored=$missingRestored",
        "restored_output_sha256=$restoredHash", "option_probe=NRD_EMBEDS_DXIL_SHADERS OFF -> ON"
    )
}

function Capture-Inventory {
    Get-ChildItem $PrimaryTree -Filter *.vcxproj -Recurse |
        ForEach-Object { $_.FullName.Substring($PrimaryTree.Length + 1) } | Sort-Object |
        Out-File -LiteralPath (Join-Path $Evidence "target-inventory.txt") -Encoding utf8
    $projects = Get-ChildItem $PrimaryTree -Filter *.vcxproj -Recurse
    $patterns = "MultiProcessorCompilation|PrecompiledHeader|DebugInformationFormat|ProgramDataBaseFileName|AdditionalOptions|LinkIncremental|PostBuildEvent|GenerateDebugInformation"
    $projects | ForEach-Object {
        Select-String -LiteralPath $_.FullName -Pattern $patterns | ForEach-Object {
            "$($_.Path.Substring($PrimaryTree.Length + 1)):$($_.LineNumber):$($_.Line.Trim())"
        }
    } | Out-File -LiteralPath (Join-Path $Evidence "generated-project-settings.txt") -Encoding utf8
}

Capture-Environment
switch ($Stage) {
    "Capture" { break }
    "Configure" { Configure-Baseline; break }
    "Build" { Build-Baseline; break }
    "Incremental" { Run-Incremental; break }
    "Deployment" { Run-DeploymentInvalidation; break }
    "Nrd" { Run-NrdInvalidation; break }
    "Invalidation" { Run-DeploymentInvalidation; Run-NrdInvalidation; break }
    "Inventory" { Capture-Inventory; break }
    "All" {
        Configure-Baseline
        Build-Baseline
        Run-Incremental
        Run-DeploymentInvalidation
        Run-NrdInvalidation
        Capture-Inventory
        break
    }
}

Write-Output "Build benchmark evidence: $Evidence"
