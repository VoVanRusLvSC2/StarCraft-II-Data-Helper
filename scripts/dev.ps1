[CmdletBinding()]
param(
    [ValidateSet("Configure", "Build", "Test", "Run")]
    [string] $Action = "Build",
    [ValidateSet("Debug", "Release")]
    [string] $Configuration = "Debug"
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
        $ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
        if (-not (Test-Path -LiteralPath $ctest)) {
            $ctest = Find-Tool "ctest" @()
        }
        Invoke-Checked $ctest @("--preset", $buildPreset)
        return
    }

    Invoke-Checked $cmake @("--build", "--preset", $buildPreset, "--target", "SC2DataHelper")
    $deployTool = if ($env:QT_ROOT) { Join-Path $env:QT_ROOT "bin\windeployqt.exe" } else { $null }
    if (-not $deployTool -or -not (Test-Path -LiteralPath $deployTool)) {
        $deployTool = Find-Tool "windeployqt" @()
    }
    Invoke-Checked $deployTool @("--$($Configuration.ToLowerInvariant())", "--no-translations", $executable)
    & $executable
} finally {
    Pop-Location
}
