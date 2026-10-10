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

In the Optimization Wizard, if reference safety checks exclude every selected
rename item, the rename step is skipped and its reasons appear in the summary.
The other selected steps can continue. Invalid rename results and file errors
still stop the batch before it saves the affected files.

## Decoration and unused objects

In the GUI, open **Map Performance**, search for map Regions by name or ID,
then select them in the list or on the map. The selected Regions are highlighted
and listed above the results. Press **Preview** to see
which placements can be converted and which stay static. **Create
Galaxy-Converted Map Copy** writes a separate `.SC2Map` with the generated
Galaxy file and keeps the source map unchanged.

**Include restore backup files (JSON/XML)** is off by default and remembered
between sessions. Enable it for a separate exact rollback backup. The normal
**Rebuild Placements from Galaxy** operation reads placement statements and
compact annotations from Galaxy without either restore sidecar. The CLI switch
is `--restore-sidecars`. Runtime actor catalog data is still included where
needed; it is not a restoration backup. Conversion also merges the exported
actors' zero-based variant indices into the map's native `Preload.xml` so their
models are requested before actor creation. Restoration restores that file too.
Standalone `--galaxy-only` exports include `.ActorData.xml` and `.Preload.xml`;
import these at `Base.SC2Data/GameData/ActorData.xml` and `Preload.xml` respectively.

The installed `SC2DecorOptimizeMap.exe` offers the same conversion from a
command line. It always writes a new `.SC2Map` unless you explicitly use
`--overwrite` for the output path.

For each selected Region, the copy removes supported doodads and simple units
with no ID/name or concrete unit-type references in scripts or GUI triggers. One Galaxy function
per Region, such as `Create_Map_1()` for a Region named `Map_1`, recreates both
without batch functions; `Clear_Map_1()` removes the generated objects. Call them from
your map triggers when the Region should appear or disappear. Unsupported or
referenced placements stay static. A lookup such as `UnitGroup("CommandCenter", ...)`
keeps all command centers in native `Objects`, so a hero-spawn trigger can still
find its anchor. Preview identifies the referencing file. Comments, unit catalog
definitions and preload entries alone do not trigger this protection. Broad
actions such as “all units in Region” without a concrete type do not count as
direct references: they will see fewer units before
`Create` and after `Clear`.

```powershell
SC2DecorOptimizeMap.exe source.SC2Map optimized.SC2Map --zones zones.json --report decor-report.json
```

The separate CLI also has an optional `--visibility-only` mode. It keeps
`Objects` byte-identical and generates
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
