[CmdletBinding()]
param(
    [ValidateSet("Configure", "Build", "Test", "Stage", "Run", "Beta2RealMapValidation", "LayoutSmoke", "MapPreviewSmoke")]
    [string] $Action = "Build",
    [ValidateSet("Debug", "Release")]
    [string] $Configuration = "Debug",
    [string] $CorpusPath = "C:\Users\Vladimir\Downloads\TriggerRivezerTests",
    [string] $RequiredMapPath = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String("QzpcUHJvZ3JhbSBGaWxlcyAoeDg2KVxTdGFyQ3JhZnQgSUlcTWFwc1zQmtCw0LzQv9Cw0L3QuNGPX9CY0LzQv9C10YDQuNGPX0tTUF/QnNC40YHRgdC40Y9fMV9PUFJJTUl6QVRJT04uU0MyTWFw")),
    [string] $MapPreviewPath = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String("QzpcUHJvZ3JhbSBGaWxlcyAoeDg2KVxTdGFyQ3JhZnQgSUlcTWFwc1zQmtCw0LzQv9Cw0L3QuNGPX9CY0LzQv9C10YDQuNGPX0tTUF/QnNC40YHRgdC40Y9fMV9PUFJJTUl6QVRJT04uU0MyTWFw")),
    [string] $DiagnosticOutputPath = "target\diag\beta2-real-maps",
    [switch] $InventoryOnly
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$preset = "windows-msvc"
$buildPreset = "windows-msvc-$($Configuration.ToLowerInvariant())"
$buildDirectory = Join-Path $projectRoot "build\windows-msvc"
$executable = Join-Path $buildDirectory "$Configuration\SC2DataHelper.exe"

function Find-Tool([string] $name, [string[]] $fallbackPaths) {
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    foreach ($candidate in $fallbackPaths) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    throw "$name was not found. Install it or add it to PATH."
}

function Invoke-Checked([string] $tool, [string[]] $arguments) {
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$([IO.Path]::GetFileName($tool)) failed with exit code $LASTEXITCODE."
    }
}

function Resolve-QtRoot {
    if ($env:QT_ROOT -and (Test-Path -LiteralPath (Join-Path $env:QT_ROOT "bin\windeployqt.exe"))) {
        return $env:QT_ROOT
    }
    $cache = Join-Path $buildDirectory "CMakeCache.txt"
    if (Test-Path -LiteralPath $cache) {
        $qt6DirLine = Select-String -Path $cache -Pattern '^Qt6_DIR:PATH=(.+)$' | Select-Object -First 1
        if ($qt6DirLine) {
            $qt6Dir = $qt6DirLine.Matches[0].Groups[1].Value
            $candidate = Split-Path (Split-Path (Split-Path $qt6Dir -Parent) -Parent) -Parent
            if (Test-Path -LiteralPath (Join-Path $candidate "bin\windeployqt.exe")) {
                return $candidate
            }
        }
    }
    throw "Qt kit was not found. Set QT_ROOT to the Qt kit root before running this command."
}

function Stage-Executable([string] $qtRoot) {
    $deployTool = Join-Path $qtRoot "bin\windeployqt.exe"
    Invoke-Checked $deployTool @("--$($Configuration.ToLowerInvariant())", "--no-translations", $executable)
}

$cmake = Find-Tool "cmake" @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

Push-Location $projectRoot
try {
    if ($Action -eq "Configure") {
        Invoke-Checked $cmake @("--preset", $preset)
        return
    }

    Invoke-Checked $cmake @("--preset", $preset)
    if ($Action -eq "Build") {
        Invoke-Checked $cmake @("--build", "--preset", $buildPreset)
        return
    }

    if ($Action -eq "Test") {
        Invoke-Checked $cmake @("--build", "--preset", $buildPreset, "--target", "SC2DataHelperTests")
        $qtRoot = Resolve-QtRoot
        $env:Path = "$(Join-Path $qtRoot 'bin');$env:Path"
        $ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
        if (-not (Test-Path -LiteralPath $ctest)) {
            $ctest = Find-Tool "ctest" @()
        }
        Invoke-Checked $ctest @("--preset", $buildPreset)
        return
    }

    if ($Action -eq "Beta2RealMapValidation") {
        Invoke-Checked $cmake @("--build", "--preset", $buildPreset, "--target", "SC2Beta2RealMapValidation")
        $qtRoot = Resolve-QtRoot
        $env:Path = "$(Join-Path $qtRoot 'bin');$env:Path"
        $validator = Join-Path $buildDirectory "$Configuration\SC2Beta2RealMapValidation.exe"
        $validatorArgs = @(
            "--corpus", $CorpusPath,
            "--required-map", $RequiredMapPath,
            "--output", $DiagnosticOutputPath
        )
        if ($InventoryOnly) {
            $validatorArgs += "--inventory-only"
        }
        Invoke-Checked $validator $validatorArgs
        return
    }

    if ($Action -eq "LayoutSmoke") {
        Invoke-Checked $cmake @("--build", "--preset", $buildPreset, "--target", "SC2DataHelper")
        $qtRoot = Resolve-QtRoot
        $env:Path = "$(Join-Path $qtRoot 'bin');$env:Path"
        $application = Join-Path $buildDirectory "$Configuration\SC2DataHelper.exe"
        $layoutOutput = Join-Path $DiagnosticOutputPath "layout-smoke"
        Invoke-Checked $application @("--layout-smoke", $layoutOutput)
        return
    }

    if ($Action -eq "MapPreviewSmoke") {
        if (-not (Test-Path -LiteralPath $MapPreviewPath -PathType Leaf)) {
            throw "Map preview source does not exist: $MapPreviewPath"
        }
        Invoke-Checked $cmake @("--build", "--preset", $buildPreset, "--target", "SC2DataHelper")
        $qtRoot = Resolve-QtRoot
        $env:Path = "$(Join-Path $qtRoot 'bin');$env:Path"
        $application = Join-Path $buildDirectory "$Configuration\SC2DataHelper.exe"
        $previewOutput = Join-Path $DiagnosticOutputPath "real-map-preview"
        $quotedPreviewPath = '"' + $MapPreviewPath.Replace('"', '\"') + '"'
        $quotedPreviewOutput = '"' + $previewOutput.Replace('"', '\"') + '"'
        $previewProcess = Start-Process -FilePath $application `
            -ArgumentList @("--map-preview-smoke", $quotedPreviewPath, $quotedPreviewOutput) `
            -Wait -PassThru -WindowStyle Hidden
        if ($previewProcess.ExitCode -ne 0) {
            throw "Map preview smoke failed with exit code $($previewProcess.ExitCode)."
        }
        $previewReport = Join-Path $previewOutput "real-map-preview.json"
        if (-not (Test-Path -LiteralPath $previewReport)) {
            throw "Map preview smoke did not create its JSON report: $previewReport"
        }
        return
    }

    Invoke-Checked $cmake @("--build", "--preset", $buildPreset, "--target", "SC2DataHelper")
    $qtRoot = Resolve-QtRoot
    Stage-Executable $qtRoot
    if ($Action -eq "Stage") {
        return
    }
    & $executable
} finally {
    Pop-Location
}
