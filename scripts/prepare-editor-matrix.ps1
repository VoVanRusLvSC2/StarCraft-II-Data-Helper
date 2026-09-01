param(
    [string] $AggregatePath = "target\diag\beta2-real-maps\aggregate-report.json",
    [string] $QueuePath = "target\diag\beta2-real-maps\editor-queue.json",
    [string] $OutputQueuePath = "target\diag\beta2-real-maps\editor-queue-prepared.json",
    [string] $CopyRoot = "target\diag\beta2-real-maps\editor-baseline-copies"
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$diagnosticRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'target\diag\beta2-real-maps'))

function Full-ProjectPath([string] $path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectRoot $path))
}

function Safe-Slug([string] $name) {
    $slug = [regex]::Replace($name.ToLowerInvariant(), '[^\p{L}\p{Nd}._-]+', '-')
    $slug = $slug.Trim('-','.')
    if ($slug.Length -gt 72) { $slug = $slug.Substring(0,72).Trim('-','.') }
    if ([string]::IsNullOrWhiteSpace($slug)) { return 'document' }
    return $slug
}

$aggregateFullPath = Full-ProjectPath $AggregatePath
$queueFullPath = Full-ProjectPath $QueuePath
$outputQueueFullPath = Full-ProjectPath $OutputQueuePath
$copyRootFullPath = Full-ProjectPath $CopyRoot
foreach ($path in @($outputQueueFullPath, $copyRootFullPath)) {
    if (-not $path.StartsWith($diagnosticRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Editor matrix artifacts must remain below $diagnosticRoot"
    }
}
if (-not (Test-Path -LiteralPath $aggregateFullPath -PathType Leaf)) { throw "Aggregate report is absent: $aggregateFullPath" }
if (-not (Test-Path -LiteralPath $queueFullPath -PathType Leaf)) { throw "Editor queue is absent: $queueFullPath" }
New-Item -ItemType Directory -Path $copyRootFullPath -Force | Out-Null

$aggregate = Get-Content -LiteralPath $aggregateFullPath -Raw -Encoding UTF8 | ConvertFrom-Json
$queue = Get-Content -LiteralPath $queueFullPath -Raw -Encoding UTF8 | ConvertFrom-Json
$candidates = New-Object Collections.ArrayList
foreach ($candidate in @($queue.candidates)) { [void]$candidates.Add($candidate) }
$sourcesWithOutput = @{}
foreach ($candidate in @($queue.candidates)) {
    $sourcesWithOutput[([string]$candidate.source_path).ToLowerInvariant()] = $true
}
$materialized = @()

foreach ($report in @($aggregate.reports)) {
    $sourcePath = [string]$report.source_path
    if ([string]::IsNullOrWhiteSpace($sourcePath) -or
        -not $report.analysis_complete_before -or
        $report.dependency_resolution -ne 'RESOLVED' -or
        -not $report.source_unchanged -or
        $sourcesWithOutput.ContainsKey($sourcePath.ToLowerInvariant())) {
        continue
    }
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { continue }
    $hashBefore = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hashBefore -ne ([string]$report.source_sha256).ToLowerInvariant()) {
        throw "Source hash no longer matches the validation manifest: $sourcePath"
    }
    $folder = Join-Path $copyRootFullPath ((Safe-Slug ([IO.Path]::GetFileNameWithoutExtension($sourcePath))) + '-' + $hashBefore.Substring(0,12))
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $copyPath = Join-Path $folder ([IO.Path]::GetFileName($sourcePath))
    if (Test-Path -LiteralPath $copyPath) {
        $existingHash = (Get-FileHash -LiteralPath $copyPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($existingHash -ne $hashBefore) { throw "Existing Editor baseline copy differs: $copyPath" }
    } else {
        Copy-Item -LiteralPath $sourcePath -Destination $copyPath
    }
    $copyHash = (Get-FileHash -LiteralPath $copyPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $hashAfter = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($copyHash -ne $hashBefore -or $hashAfter -ne $hashBefore) {
        throw "Editor baseline materialization failed hash verification: $sourcePath"
    }
    $candidate = [ordered]@{
        source_path = $sourcePath
        copy_path = $copyPath
        operation = 'baseline-byte-copy'
        required_mission = [bool]$report.required_mission
        category = [string]$report.category
        complex_name = [bool]$report.complex_name
        document_kind = [string]$report.document_kind
        editor_acceptance = 'NOT_RUN'
    }
    [void]$candidates.Add($candidate)
    $materialized += [ordered]@{
        source_path = $sourcePath
        source_sha256_before = $hashBefore
        source_sha256_after = $hashAfter
        source_unchanged = $true
        copy_path = $copyPath
        copy_sha256 = $copyHash
    }
}

$prepared = [ordered]@{
    schema = 'sc2dh.beta2.editor-queue-prepared.v1'
    created_utc = [DateTime]::UtcNow.ToString('o')
    timeout_seconds = 200
    note = 'Optimized/compressed candidates are preferred. Baseline byte-copies fill only missing representative categories and do not validate an optimization.'
    candidates = @($candidates)
    baseline_materialization = $materialized
}
$prepared | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputQueueFullPath -Encoding UTF8
$prepared | ConvertTo-Json -Depth 8
