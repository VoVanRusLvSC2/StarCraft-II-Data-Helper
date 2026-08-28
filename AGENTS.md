# Development guide for coding agents

## Fast, reproducible workflow

This is a Windows C++20 / Qt 6 project. From PowerShell, set the Qt kit root once
per session, then use the project script instead of hard-coded local paths:

```powershell
$env:QT_ROOT = 'C:\Qt\6.8.2\msvc2022_64'
.\scripts\dev.ps1 -Action Test -Configuration Debug
.\scripts\dev.ps1 -Action Run -Configuration Debug
```

The script configures the `windows-msvc` CMake preset, builds the requested
target, and runs CTest when requested. `QT_ROOT` must contain `bin\windeployqt.exe`.

## Safe handling of SC2 content

- Treat `.SC2Map`, `.SC2Mod`, and `.SC2Campaign` as user data.
- Analyze and preview first. Do not run an apply/delete/rename operation on an
  original archive unless the user explicitly authorizes it.
- Use copies in a temporary or explicitly named output directory for CLI work.
- Preserve existing backup and rollback behavior; do not weaken it merely to
  make an automated flow shorter.

## Verification

- Run the smallest relevant test first, then `scripts/dev.ps1 -Action Test` for
  changes to shared analysis, archive, or mutation code.
- Keep new regression coverage alongside `tests/test_core.cpp` until the test
  suite is split into domain files.
- Do not claim the StarCraft II Editor accepts an output unless that editor
  check was actually performed.

## Project map

- `src/core`: archive handling, indexing, analysis, optimization, and safety.
- `src/app` and `src/ui`: Qt application and workflows.
- `src/tools`: command-line helpers. Their input/output behavior must stay
  explicit and safe for automation.
- `tests/test_core.cpp`: Qt regression suite.
