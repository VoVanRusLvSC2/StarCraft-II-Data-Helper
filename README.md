# StarCraft II Data Helper

StarCraft II Data Helper is a Windows desktop tool for analyzing and optimizing
StarCraft II catalog XML in `.SC2Map`, `.SC2Mod`, extracted component folders,
and standalone XML files.

Main goal: make SC2 data cleanup safer and faster without hand-editing every
catalog file.

[Download the latest build from Releases](https://github.com/VoVanRusLvSC2/StarCraft-II-Data-Helper/releases)

## What it does

- Analyze XML catalogs, references, duplicate bodies, unused chains, assets,
  localization, and archive metadata
- Build and update typed Data Collections for supported editor catalog objects
- Preview and safely remove unused data objects and whole unused data-object chains
- Find exact duplicate XML bodies, redirect references, and merge safely
- Preview and apply Rename To Standard operations for unit-family style data
- Run Import Cleanup for unused imported files in folders or archives
- Run Deep Cleanup for stale strings, redundant default fields, broken actor
  events, dependency review, and archive/helper trash
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
- default to `UnitAbilWeapon` mode, creating separate Unit, Ability, and Weapon
  collections where the data supports it

The tool works with real SC2 object IDs. It can also handle non-standard naming
when the map already contains existing custom IDs outside the `CollectionID@Child`
style.

## Safety

Destructive operations are preview-first and backup-first.

- No blind automatic deletion
- Duplicate merge redirects references before delete
- Unused-object cleanup skips unsafe rows instead of stopping the whole batch
- Rollback is used on apply failure
- Archive mode stays conservative when binary references cannot be proven
- Data Collection records are preserved even when related objects are deleted

## Main tools

### Unused Data Objects

Find catalog data objects with no incoming XML references and no script/text usage.
Whitelisted, protected, and referenced objects are blocked. Linked unused
subgraphs can be removed as chains when every incoming XML source is also part
of the selected deletion set.

### Duplicate Merge

Detect exact duplicate normalized XML bodies for the same object type, preview
reference redirects, then keep one object and remove the duplicate safely.
Duplicate Merge is enabled by default in the Optimization Wizard.

### Import Cleanup

Find unused imported files separately from catalog object cleanup. This covers
assets such as `.dds`, `.m3`, `.ogg`, `.wav`, `.tga`, `.layout`, and related
files when they are not referenced by Data XML, layouts, triggers, preload
data, or game strings. In archive mode the wizard materializes cleanup-relevant
archive entries into its workspace before apply, so direct `.SC2Map` and
`.SC2Mod` optimization can remove proven unused imports from the packed file.
Editor-managed map files such as `Minimap.tga`, `LightingMap.tga`, and
`PreloadAssetDB.txt` are protected even when normal XML references do not point
to them.

### Deep Cleanup

Find and optionally apply safe cleanup outside data-object and import deletion:

- stale localization lines for object IDs that no longer exist
- redundant attributes that duplicate a local parent object's value
- broken actor event XML nodes that reference only missing typed IDs
- temporary, report, backup, and pending helper files
- dependency metadata as review-only candidates

Dependency removal is intentionally review-only because hidden editor/runtime
links can break a map even when XML references look unused.

### Rename To Standard

Preview and apply rename plans for unit-family style data, including archive
mode. This remains review-first because many maps use custom naming rules.

### Optimization Wizard

Build one batch plan, review every step, apply changes, refresh analysis, then
close the wizard. One unsafe unused-chain row no longer cancels the whole apply;
it is skipped and reported while the rest of the selected safe work continues.

## Typical usage

1. Open an `.SC2Map`, `.SC2Mod`, folder, or XML file
2. Run `Analyze`
3. Review Data Collection, Unused Data Objects, Import Cleanup, Duplicate
   Merge, or Optimization
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
