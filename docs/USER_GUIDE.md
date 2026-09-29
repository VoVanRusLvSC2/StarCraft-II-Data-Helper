# SC2 Data Helper — User Guide

## What the tool changes

SC2 Data Helper analyzes StarCraft II XML catalogs and archive entries. Every
destructive action is shown as a preview and can create a backup first.

Supported inputs are `.SC2Map`, `.SC2Mod`, component folders, and standalone
catalog XML files.

## Recommended workflow

1. Open the map, mod, folder, or XML file.
2. Run **Analyze** and inspect parse errors and dependencies.
3. Review **Data Collection** before cleanup. Existing records are preserved.
4. Use **Unused Data Objects** for catalog objects and **Import Cleanup** for
   files such as textures, sounds, layouts, and M3 models.
5. Use **Duplicate Merge** only after checking the reference preview.
6. Apply changes with backup enabled.
7. Open the result in StarCraft II Editor and test the affected maps.

## Decoration and unused objects

The installed `SC2DecorOptimizeMap.exe` handles placed map decoration. It is a
command-line tool, separate from the GUI. It always writes a new `.SC2Map`
unless you explicitly use `--overwrite` for the output path.

For a selected zone, actor recreation removes supported, unreferenced doodad
placements from the new map's `Objects` entry and generates Galaxy functions
such as `NAME_OUT_FUNK_Create_1()` and `NAME_OUT_FUNK_Clear_1()`. Call these
functions from your map triggers when the zone should appear or disappear.
Unsupported or referenced placements stay static.

```powershell
SC2DecorOptimizeMap.exe source.SC2Map optimized.SC2Map --zones zones.json --report decor-report.json
```

If you need to hide and restore the *original* placed doodads, use
`--visibility-only`. This keeps `Objects` byte-identical and generates
`NAME_OUT_FUNK_Hide_1()` and `NAME_OUT_FUNK_Restore_1()` for your triggers.
It does not remove those placements from the map.

```powershell
SC2DecorOptimizeMap.exe source.SC2Map visibility.SC2Map --zones zones.json --visibility-only --report visibility-report.json
```

`zones.json` is an array such as
`[{"id":1,"name":"Area 1","xMin":0,"yMin":0,"xMax":50,"yMax":50}]`.
`--map-regions` can use supported exact map regions instead of `--zones`.
Open the generated map in StarCraft II Editor and test the trigger calls before
using it as your final map.

The GUI's **Unused Data Objects** tab is a different operation: it previews
and deletes verified unused *catalog data objects*. It does not delete placed
doodads. Entries marked `Unknown` or `Blocked` cannot be deleted there.

## Data Collection

The builder creates the editor schema first (`CDataCollectionPattern` and
default collection templates), then creates a concrete collection and adds
real `DataRecord` entries. Patterns control editor fields; they do not replace
records for custom effects, validators, behaviors, or requirements.

The application always uses the optimized `UnitAbilWeapon` layout: separate
Unit, Ability, and Weapon roots. There is no mode selector. Shared objects get
one canonical owner in newly generated collections, while existing legacy
records are preserved unless explicitly reviewed.

Plans above the automatic scale limit are marked **Review large plan** and are
not selected automatically. The batch wizard previews the first 200 families
and summarizes the remainder; use the dedicated Data Collection tab for an
individual family. Other optimization steps are not blocked by this limit.

## Asset and M3 preview

Image assets can be previewed directly. M3 files can be rotated with the left
mouse button and zoomed with the mouse wheel. The viewer reads static geometry,
UV coordinates, skin weights, bone hierarchy and rest transforms, animation
sequence metadata, standard materials, diffuse layers and texture paths.
Materials are shown as separate shaded colors, and a referenced texture is
previewed alongside the model when it exists in the opened project.

The current renderer does not yet evaluate `STC/STG` skeletal animation curves
or map DDS pixels onto triangles. Those files remain untouched and the preview
falls back to static geometry plus the decoded texture pane.

## Updates and installation

The application checks the latest GitHub release at startup and offers to open
its release page only when the published semantic version is newer. Official
release publication triggers the Windows installer workflow.

## Safety

Do not delete dependencies unless you understand the map's dependency graph.
Binary references cannot always be proven from XML alone. Always test the
optimized copy in the StarCraft II Editor before distributing it.

When a `.SC2Mod` or `.SC2Campaign` is opened by itself, its exported catalog IDs
and apparently unused imports may have consumers in another map. The tool keeps
those deletions and ID renames blocked. Analyze the complete consumer project or
perform an explicit project-wide review before changing that public interface.

**Closed Project / Aggressive standardization** in Settings removes that block,
raises the Data Collection batch limit, standardizes uniquely owned child IDs
to `CollectionID@Role`, and then creates Patterns and Records. It can break any
external consumer that is not updated together, so keep backups enabled.

## Building

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Windows executable is written to `build/Release/SC2DataHelper.exe`.
