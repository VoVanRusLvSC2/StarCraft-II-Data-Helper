param(
    [string] $QueuePath = "target\diag\beta2-real-maps\editor-queue.json",
    [string] $OutputPath = "target\diag\beta2-real-maps\editor-oracle",
    [string] $EditorPath = "C:\Program Files (x86)\StarCraft II\Support64\SC2Editor_x64.exe",
    [ValidateRange(1, 200)] [int] $TimeoutSeconds = 200,
    [ValidateRange(1, 64)] [int] $MaximumDocuments = 12
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Sc2DhEditorCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
"@

function Save-EditorWindow([IntPtr] $handle, [string] $path) {
    if ($handle -eq [IntPtr]::Zero) { return $false }
    $rect = New-Object Sc2DhEditorCapture+RECT
    if (-not [Sc2DhEditorCapture]::GetWindowRect($handle, [ref] $rect)) { return $false }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 2 -or $height -lt 2) { return $false }
    $bitmap = New-Object Drawing.Bitmap($width, $height)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $hdc = $graphics.GetHdc()
            try { $captured = [Sc2DhEditorCapture]::PrintWindow($handle, $hdc, 2) }
            finally { $graphics.ReleaseHdc($hdc) }
        } finally { $graphics.Dispose() }
        if (-not $captured) { return $false }
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        return $true
    } finally { $bitmap.Dispose() }
}

function Safe-Slug([string] $name) {
    $slug = [regex]::Replace($name.ToLowerInvariant(), '[^\p{L}\p{Nd}._-]+', '-')
    $slug = $slug.Trim('-','.')
    if ($slug.Length -gt 72) { $slug = $slug.Substring(0,72).Trim('-','.') }
    if ([string]::IsNullOrWhiteSpace($slug)) { return 'document' }
    return $slug
}

function Existing-EditorProcesses {
    return @(Get-Process SC2Editor_x64,SC2Editor -ErrorAction SilentlyContinue)
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$diagnosticRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'target\diag\beta2-real-maps'))
function Full-ProjectPath([string] $path) {
    if ([IO.Path]::IsPathRooted($path)) { return [IO.Path]::GetFullPath($path) }
    return [IO.Path]::GetFullPath((Join-Path $projectRoot $path))
}
$queueFullPath = Full-ProjectPath $QueuePath
$preparedQueue = Join-Path $diagnosticRoot 'editor-queue-prepared.json'
if ($QueuePath -eq 'target\diag\beta2-real-maps\editor-queue.json' -and
    (Test-Path -LiteralPath $preparedQueue -PathType Leaf)) {
    $queueFullPath = $preparedQueue
}
$outputFullPath = Full-ProjectPath $OutputPath
if (-not $outputFullPath.StartsWith($diagnosticRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Editor oracle output must remain below $diagnosticRoot"
}
if (-not (Test-Path -LiteralPath $queueFullPath -PathType Leaf)) { throw "Editor queue is absent: $queueFullPath" }
if (-not (Test-Path -LiteralPath $EditorPath -PathType Leaf)) { throw "SC2 Editor is absent: $EditorPath" }
New-Item -ItemType Directory -Path $outputFullPath -Force | Out-Null

$queue = Get-Content -LiteralPath $queueFullPath -Raw -Encoding UTF8 | ConvertFrom-Json
$available = @($queue.candidates)
$selected = New-Object Collections.ArrayList
$seenSources = @{}
$groupSpecs = @(
    [pscustomobject]@{ Items=@($available | Where-Object {$_.required_mission}); Limit=1 },
    [pscustomobject]@{ Items=@($available | Where-Object {$_.category -eq 'small'}); Limit=2 },
    [pscustomobject]@{ Items=@($available | Where-Object {$_.category -eq 'medium'}); Limit=2 },
    [pscustomobject]@{ Items=@($available | Where-Object {$_.category -eq 'large'}); Limit=2 },
    [pscustomobject]@{ Items=@($available | Where-Object {$_.complex_name}); Limit=2 },
    [pscustomobject]@{ Items=@($available | Where-Object {$_.document_kind -eq 'SC2Mod'}); Limit=1 },
    [pscustomobject]@{ Items=$available; Limit=$MaximumDocuments }
)
foreach ($spec in $groupSpecs) {
    $addedForGroup = 0
    foreach ($item in $spec.Items) {
        if ($selected.Count -ge $MaximumDocuments -or $addedForGroup -ge $spec.Limit) { break }
        $sourceKey = ([string]$item.source_path).ToLowerInvariant()
        if ($seenSources.ContainsKey($sourceKey)) { continue }
        $seenSources[$sourceKey] = $true
        [void]$selected.Add($item)
        ++$addedForGroup
    }
}
$candidates = @($selected)
$matrixComplete = $candidates.Count -ge 9 `
    -and @($candidates | Where-Object {$_.required_mission}).Count -ge 1 `
    -and @($candidates | Where-Object {$_.category -eq 'small'}).Count -ge 2 `
    -and @($candidates | Where-Object {$_.category -eq 'medium'}).Count -ge 2 `
    -and @($candidates | Where-Object {$_.category -eq 'large'}).Count -ge 2 `
    -and @($candidates | Where-Object {$_.complex_name}).Count -ge 2 `
    -and @($candidates | Where-Object {$_.document_kind -eq 'SC2Mod'}).Count -ge 1
$results = @()

if ((Existing-EditorProcesses).Count -gt 0) {
    $results += [ordered]@{
        editor_acceptance = 'BLOCKED_EDITOR_IN_USE'
        detail = 'An Editor process not owned by this runner already exists. It was not controlled or closed.'
    }
} else {
    foreach ($candidate in $candidates) {
        if ((Existing-EditorProcesses).Count -gt 0) {
            $results += [ordered]@{ source_path=$candidate.source_path; copy_path=$candidate.copy_path; editor_acceptance='BLOCKED_EDITOR_IN_USE' }
            break
        }
        $candidatePath = [IO.Path]::GetFullPath([string]$candidate.copy_path)
        if (-not $candidatePath.StartsWith($diagnosticRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
            $results += [ordered]@{ source_path=$candidate.source_path; copy_path=$candidate.copy_path; editor_acceptance='BLOCKED_INVALID_DIAGNOSTIC_COPY' }
            continue
        }
        $inputHash = (Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $folder = Join-Path $outputFullPath ((Safe-Slug ([IO.Path]::GetFileNameWithoutExtension($candidatePath))) + '-' + $inputHash.Substring(0,12))
        New-Item -ItemType Directory -Path $folder -Force | Out-Null
        $oracleCopy = Join-Path $folder ([IO.Path]::GetFileName($candidatePath))
        if (Test-Path -LiteralPath $oracleCopy) {
            $existingHash = (Get-FileHash -LiteralPath $oracleCopy -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($existingHash -ne $inputHash) { throw "Existing oracle copy hash differs: $oracleCopy" }
        } else {
            Copy-Item -LiteralPath $candidatePath -Destination $oracleCopy
        }
        $copyHash = (Get-FileHash -LiteralPath $oracleCopy -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($copyHash -ne $inputHash) { throw "Oracle materialization hash differs: $oracleCopy" }

        $quotedCopy = '"' + $oracleCopy.Replace('"','\"') + '"'
        $owned = Start-Process -FilePath $EditorPath -ArgumentList @($quotedCopy) -PassThru
        $started = Get-Date
        $stableSince = $null
        $title = ''
        $acceptance = 'TIMEOUT'
        $detail = 'Editor did not reach a stable interactive window before timeout.'
        while (((Get-Date) - $started).TotalSeconds -lt $TimeoutSeconds) {
            Start-Sleep -Milliseconds 500
            $owned.Refresh()
            if ($owned.HasExited) {
                $acceptance = 'FAIL_EDITOR_EXIT'
                $detail = "Editor exited with code $($owned.ExitCode)."
                break
            }
            $title = $owned.MainWindowTitle
            $errorWordRu = [string]([char]0x041E)+[char]0x0448+[char]0x0438+[char]0x0431+[char]0x043A+[char]0x0438
            if ($title -match '(?i)error|failed|dependency' -or $title.Contains($errorWordRu)) {
                $acceptance = 'FAIL_EDITOR_ALERT'
                $detail = "Editor displayed an error/alert window: $title"
                break
            }
            if ($owned.Responding -and $owned.MainWindowHandle -ne [IntPtr]::Zero -and -not [string]::IsNullOrWhiteSpace($title)) {
                if ($null -eq $stableSince) { $stableSince = Get-Date }
                if (((Get-Date) - $stableSince).TotalSeconds -ge 10) {
                    $acceptance = 'OPENED_INTERACTIVE'
                    $detail = 'Editor reached a responsive stable window without an automatic error dialog. Trigger compilation and runtime are not proven by this signal.'
                    break
                }
            } else { $stableSince = $null }
        }

        $owned.Refresh()
        $screenshotPath = Join-Path $folder 'editor-window.png'
        $screenshotSaved = $false
        if (-not $owned.HasExited) { $screenshotSaved = Save-EditorWindow $owned.MainWindowHandle $screenshotPath }
        $results += [ordered]@{
            source_path = $candidate.source_path
            tested_copy = $oracleCopy
            tested_copy_sha256 = $copyHash
            operation = $candidate.operation
            editor_acceptance = $acceptance
            trigger_validation = 'NOT_PROVEN'
            runtime_acceptance = 'NOT_RUN'
            elapsed_seconds = [math]::Round(((Get-Date)-$started).TotalSeconds, 3)
            window_title = $title
            screenshot_saved = $screenshotSaved
            screenshot = if ($screenshotSaved) { $screenshotPath } else { $null }
            detail = $detail
        }

        if (-not $owned.HasExited) {
            [void]$owned.CloseMainWindow()
            if (-not $owned.WaitForExit(15000)) {
                $actual = Get-CimInstance Win32_Process -Filter "ProcessId=$($owned.Id)"
                if ($actual -and [IO.Path]::GetFullPath($actual.ExecutablePath) -eq [IO.Path]::GetFullPath($EditorPath)) {
                    $owned.Kill()
                    $owned.WaitForExit(5000)
                }
            }
        }
    }
}

$report = [ordered]@{
    schema = 'sc2dh.beta2.editor-oracle.v1'
    created_utc = [DateTime]::UtcNow.ToString('o')
    timeout_seconds = $TimeoutSeconds
    available_candidates = $available.Count
    selected_distinct_documents = $candidates.Count
    representative_matrix_complete = $matrixComplete
    acceptance_rule = 'OPENED_INTERACTIVE proves only a responsive Editor window; trigger/runtime remain separate and are never inferred as PASS.'
    results = $results
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputFullPath 'editor-oracle.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 8
