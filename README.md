# StarCraft II Data Helper

StarCraft II Data Helper is a Windows desktop tool for analyzing and optimizing
StarCraft II catalog XML in `.SC2Map`, `.SC2Mod`, extracted component folders,
and standalone XML files.

Main goal: make SC2 data cleanup safer and faster without hand-editing every
catalog file.

[Download the latest build from Releases](https://github.com/VoVanRusLvSC2/StarCraft-II-Data-Helper/releases)

## What it does

- Analyze XML catalogs, references, duplicate bodies, and unused objects
- Build and update Data Collections for supported editor catalog objects
- Preview and safely remove unused objects with backup-first flow
- Find exact duplicate XML bodies, redirect references, and merge safely
- Preview Rename To Standard operations for unit-family style data
- Show full XML source with syntax highlighting
- Open an Optimization Wizard for batch review before apply

## Main feature: Data Collection

The tool can create or update `DataCollectionData.xml` and preserve existing
collections instead of recreating the file from scratch.

Supported workflow:

- detect related families from real catalog links
- reuse existing `CDataCollection`, `CDataCollectionUnit`,
  `CDataCollectionAbil`, and `CDataCollectionUpgrade`
- add missing `DataRecord` entries for supported real objects
- keep existing records, metadata, and unrelated XML nodes
- update archive `(listfile)` when needed

The tool works with real SC2 object IDs. It can also handle non-standard naming
when the map already contains existing custom IDs outside the `CollectionID@Child`
style.

## Safety

Destructive operations are preview-first and backup-first.

- No automatic deletion
- Duplicate merge redirects references before delete
- Unused-object cleanup is manual only
- Rollback is used on apply failure
- Archive mode stays conservative when binary references cannot be proven

## Main tools

### Unused Objects

Find catalog objects with no incoming XML references and no script/text usage.
Whitelisted, protected, and referenced objects are blocked.

### Duplicate Merge

Detect exact duplicate normalized XML bodies for the same object type, preview
reference redirects, then keep one object and remove the duplicate safely.

### Rename To Standard

Preview rename plans for unit-family style data. This remains review-first
because many maps use custom naming rules.

### Optimization Wizard

Build one batch plan, review every step, apply changes, refresh analysis, then
close the wizard.

## Typical usage

1. Open an `.SC2Map`, `.SC2Mod`, folder, or XML file
2. Run `Analyze`
3. Review Data Collection, Unused Objects, Duplicate Merge, or Optimization
4. Preview the change
5. Apply only after checking the result

## Build from source

Requirements:

- Windows 10 or Windows 11
- CMake 3.24+
- Qt 6.5+ with `Widgets`, `Concurrent`, and `Test`
- Visual Studio 2022 or another C++20 compiler

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Release executable:

`build/Release/SC2DataHelper.exe`

## Notes

- Always test the optimized copy in StarCraft II Editor
- XML analysis cannot prove every binary/runtime reference inside archives
- This is a community project and is not affiliated with Blizzard Entertainment
