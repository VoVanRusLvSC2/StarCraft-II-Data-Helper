# StarCraft II Data Helper

> **4.0 Beta 2.** Основная функция оптимизатора — преобразование размещённого декора и юнитов в Galaxy с обратным восстановлением разметки из скрипта. JSON/XML для восстановления по умолчанию выключены и доступны как дополнительный режим в настройках.
>
> **Primary workflow:** convert placed decor and eligible units to Galaxy, then rebuild native placements from that script. This release is a **Beta**.

StarCraft II Data Helper is a Windows desktop map optimizer with reversible
decor/unit conversion to Galaxy. It also analyzes and optimizes catalog XML in
`.SC2Map`, `.SC2Mod`, extracted component folders, and standalone XML files.

The main workflow is in **Map Performance**: preview placements, create an
optimized map copy, and rebuild placements from its generated Galaxy when needed.

[Download the latest build from Releases](https://github.com/VoVanRusLvSC2/StarCraft-II-Data-Helper/releases)

## What it does

- Analyze XML catalogs, references, duplicate bodies, unused chains, assets,
  localization, and archive metadata
- Build and update typed Data Collections for supported editor catalog objects
- Preview and safely remove unused data objects and whole unused data-object chains
- Find exact duplicate XML bodies, redirect references, and merge safely
- Preview and apply Rename To Standard operations for unit-family style data
- Run Import Cleanup for unused imported files in folders or archives
- Run Deep Cleanup for stale strings, redundant default fields/nodes, broken
  actor events, dependency review, and archive/helper trash
- Scan binary model-like imports for embedded asset references before deleting
  textures, sounds, layouts, or models
- Preview M3 geometry interactively and inspect standard materials, diffuse
  texture paths, UVs, skin weights, bones, rest transforms, and animation names
- Review asset-size/duplicate audits, trigger/Galaxy performance hotspots, and
  semantic duplicate data-object candidates
- Select real map Regions in Map Performance, preview eligible decor and units,
  and create a separate map copy with placements converted to callable Galaxy
  create/clear functions
- Show full XML source with syntax highlighting
- Open an Optimization Wizard for batch review before apply
- Check GitHub Releases for newer semantic versions; release builds also have a
  portable ZIP and an automatically built Windows installer

## Map Performance — primary workflow (4.0 Beta 2)

Map Performance is complete for the Region-based workflow: select Regions,
review the exact placements and **Keep in map** choices, then create an optimized
copy of the map. Each selected Region gets one `Create_<RegionName>()`
function that recreates its removed decor and eligible units; call it from your
GUI trigger when that area is needed. The matching `Clear_<RegionName>()`
removes the objects created by that function. Nothing is created automatically
at map start. The original map is left intact.
Large Regions use small internal `DecorOpt_CreatePart_*` helpers to stay within
Galaxy's function-size limit; triggers still call just one Create function per
Region.
The generated Galaxy code reapplies model variation and team color and uses the
placed object's world position, rotation, scale, and tint. Editor-only
HeightOffset and ForcePlacement flags do not by themselves exclude decor.
Nullable tint (`NULL HDR`) is retained as a native nullable color; it is never
replaced with explicit white. This matters for weather emitters and lights with
large HDR multipliers. Terrain surface objects remain in Objects: runtime visual
actors cannot restore native terrain/pathing or doodad-enumeration membership.
Only actual `TerrainObject*` types and local catalog terrain-object membership
are treated as surfaces. Ordinary cliffs, platforms, roads and ground-cover
doodads remain eligible for conversion; visual appearance is not a surface
classification. Local catalog footprint dependencies are checked separately. Terrain
archive components are preserved. Direct placement references in triggers/scripts
keep the referenced decor or unit in the map. Stock animation, visibility and
destruction commands for a doodad type/Region use ActorRegionSend, which can
address recreated actors with their original catalog ID and Doodad alias. Those
group commands do not exclude every placement of the type. Unknown uses of a
doodad-type string in executable Galaxy conservatively keep that type native.
Galaxy comments do not count as type references. Computed type names/custom
enumeration wrappers require manual **Keep in map** exclusions. A concrete unit
type used in GUI triggers or Galaxy keeps every placed unit of that type in
`Objects`, including type-based group lookups used to find a hero's spawn anchor.
An unfiltered query for all units does not automatically exclude every type.
CLI `--keep-doodad-id` and `--keep-doodad-type` offer repeatable explicit exclusions
when a whole cinematic area should stay native.
The runtime uses the actor returned directly by `ActorCreate`, so creation of
child actors cannot redirect placement messages. Rotation,
pitch, and roll are converted from Objects radians to an explicit forward/up
basis and sent through NativeLib `SetRotation`, including zero rotation. The
full basis initializes the placement source before construction and is also
applied directly to the returned model actor after construction; all three
axes are retained for compound rotations.
XML `Pitch` rotates around model X and `Roll` around model Y, in the order
`Rz(Rotation) * Rx(Pitch) * Ry(Roll)`. The earlier mapping swapped Pitch and
Roll, which tilted compound placements incorrectly even though a reverse
conversion could restore the original XML exactly. Regression checks now cover
each axis separately as well as KSP-3's large temple wall.
The placed-doodad rotation uses local model axes, while the actor-message parser
uses local -Y as forward. The conversion preserves that distinction; using
`MakeModelFaceAngle` directly adds an unwanted 90-degree model rotation.
Regression checks reconstruct the model orientation using the message parser's
basis and compare it with the placed-doodad loader's quaternion composition.
Model variant 0 is selected explicitly. Animation initialization belongs to the
original actor's runtime catalog: inherited events, event macros, animation
brackets, global loops, speeds and fog behavior remain under the game's control.
The export does not replace them with `EditorAnim` (an editor preview selector),
force a generic `Stand`, or replay creation events and duplicate their effects.
The model and its zero-based variant are passed as the first two content
parameters of `ActorCreate`, before model initialization. For explicit variants
of multi-variant models, the runtime also sends `ModelSwap` with that same model
link and index. Converted maps merge the exported actor/variant pairs into native
`Preload.xml`, preserving the source's other preload entries.
The placement loader also initializes position and rotation before constructing
the doodad's cached fog bounds. Creating at the origin and moving afterwards
leaves those bounds at the origin, which can pause animations and particles
even when the visible model has moved into the player's view.
Map conversion now adds a placement site and CopySource fields to each exported
actor type and copies initial bearings in a separate actor scope. Original actor
IDs and inherited token/event identity are retained (including `##id##` links).
Initial transfer alone does not pin a doodad's world coordinates: its native
position getter can fall back to the scope position plus a height offset.
The exporter now explicitly sets XYZ on the created actor as well as the
placement source, before applying its full rotation and scale. This preserves
negative absolute heights for below-surface decor. Numeric/roundtrip checks
verify saved data, not the resulting appearance in the game.
No animation or fog flags are overridden. Clearing a region destroys its scopes
and their child actors. The generated Galaxy requires these fields in the
converted map's ActorData.xml; copying the Galaxy file alone is insufficient.
Restore returns the original actor catalog byte for byte (or removes it if
the source had none), and refuses to overwrite catalog edits made after export.
`Stand`, `Stand 01` and `Stand 02` are model animation clips, separate from the
placed model variation. Their choice remains with the original actor.
This path applies to all exported doodad types, including wind socks and snow
effects.
The runtime reads the effective Model link from the game catalog, so dependency
and custom-mod overrides do not depend on a particular local cache snapshot.
Exact random animation phase is not preserved. A convert/restore comparison
verifies saved placement data; it does
not execute Galaxy or prove that the game renders those placements identically.
Footprint/pathing objects, unknown placement fields, and referenced objects
stay in `Objects`; Preview shows the reason. Visual equivalence still requires
an Editor and in-game check for each map.

**Choose units to keep in Objects...** opens a searchable list grouped by unit
type. Check one unit or its type to keep it in `Objects`; units protected by
the safety rules are shown as already kept. The CLI supports repeatable
`--keep-unit-id <id>` and `--keep-unit-type <type>` options.

`SC2DecorOptimizeMap <converted> <restored> --restore` now imports native
placements from the executable SC2DataHelper Galaxy statements, without reading
a JSON Objects backup. XYZ comes from the absolute-world transform passed to
the creation helper. Original angle literals are preserved; older exports
without them are restored from the forward/up basis. Scale,
variant, team color, tint and units come from the creation/tint statements.
One compressed, short-line header block carries only object identity and editor
flags, not transforms; creation functions contain no per-object comments.
Incomplete, unsupported or duplicate-ID imports fail without creating a map.
Re-export older maps to obtain these identity annotations. The separate exact
backup rollback API remains available internally.

Restore JSON/XML files are **off by default**. The GUI checkbox **Include restore
backup files (JSON/XML)** persists this preference; the CLI equivalent is
`--restore-sidecars`. Without these files, Galaxy keeps the compact identity,
flags and actor-catalog cleanup annotations needed for a round trip. It does
not contain a duplicate Objects backup. The runtime `ActorData.xml` patch is
still required for initial world placement and is separate from restore
sidecars. Do not delete sidecars from older exports whose annotations were
stored externally; regenerate those maps instead.

The restored copy keeps public Galaxy entry points as no-op compatibility
functions, so existing GUI triggers compile without spawning duplicate actors.
Runtime recreation passes one literal transform string to the shared
creation helper, which sends `SetPosition` and `SetRotation` separately,
preserving absolute negative Z and avoiding additional Galaxy
fixed-point rounding. Before setting XYZ/rotation, each created doodad receives
`HostSiteOpsSet ::Host` with an empty operation list and held position/rotation.
This bypasses host placement adaptation on the created instance; native objects
of the same type keep their catalog host operations. `ActorCreate` has no
unit-style ignore-placement creation flag. This establishes explicit world bearings; an actual game
comparison is still required for catalog events, host operations and cinematic
behavior.

Runtime scope/unit arrays use disjoint slices sized by the total exported
placements, rather than multiplying the largest region by the largest region
ID. The helper uses the same bearings message before and after creation,
avoiding duplicate XYZ/rotation constants. Catalog rollback data is split into
short header lines as well. These checks reduce compiler pressure; passing the
application tests is not an in-game Galaxy compilation check.

Explicit model variants use native `Preload.xml` actor entries with zero-based
`Variations` indices. Computed runtime `PreloadModel` calls are not used.
Galaxy-only export includes the matching `.Preload.xml` and actor data, which
must accompany the script. Restore verifies and restores the original preload.

For overlapping Regions, a placement belongs to the smaller Region. A unit
whose only extra editor flag is `ForcePlacement` is recreated with placement
restrictions ignored. Other unsupported unit fields keep that unit in `Objects`,
with the reason shown in Preview.

Only simple unit placements that can be recreated safely are moved. A direct
trigger or script reference to a **specific unit** (by its ID or name) keeps
that unit in the map. A reference to its **unit type**, such as
`UnitGroup("CommandCenter", ...)`, keeps all placements of that type; Preview
names the referencing file. Comments, catalog definitions and preload entries
alone do not count as trigger references. Generic operations that enumerate
**all units in a Region**, without a concrete type, do not block conversion.
Such operations may see different
units before and after you call `Create_<RegionName>()`; check the
timing of your triggers and use **Keep in map** for units that must exist from
map start. Preview the result before applying the copy.

Нюанс для юнитов: блокирует перенос только прямая ссылка на конкретный юнит
по ID или имени. Действие триггера «все юниты в регионе» само по себе не
блокирует перенос каждого юнита. До вызова `Create_<имя региона>()` такие
юниты отсутствуют в копии карты; при необходимости оставьте конкретный юнит
через **Keep in map**.

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

`UnitAbilWeapon` is the single optimized mode used by the application. Shared
unscoped objects receive one deterministic canonical owner instead of being
copied into every reachable collection. Unique owned child IDs can be
standardized as `CollectionID@Role`; remaining shared or non-standard objects
stay linked through explicit `DataRecord` entries.

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
- Standalone `.SC2Mod`/`.SC2Campaign` sources protect public catalog IDs and
  imports that may be consumed by maps or mods outside the current analysis

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
archive entries into its workspace before apply. A direct `.SC2Map` can remove
proven unused imports from the packed file. For a standalone `.SC2Mod` or
`.SC2Campaign`, imports with no local reference remain review-only because an
external consumer can still use them.
Editor-managed map files such as `Minimap.tga`, `LightingMap.tga`, and
`PreloadAssetDB.txt` are protected even when normal XML references do not point
to them.

Binary model-like files such as `.m3` are also scanned for printable embedded
asset paths. This protects textures and related imports that are referenced only
inside model payloads rather than in XML.

### Deep Cleanup

Find and optionally apply safe cleanup outside data-object and import deletion:

- stale localization lines for object IDs that no longer exist
- redundant attributes that duplicate a local parent object's value
- redundant direct XML child nodes that duplicate a local parent object's node
- broken actor event XML nodes that reference only missing typed IDs
- temporary, report, backup, and pending helper files
- dependency metadata as review-only candidates

Dependency removal is intentionally review-only because hidden editor/runtime
links can break a map even when XML references look unused.

Deep Cleanup also reports review-only optimization hints that are useful for
manual work but are not safe to auto-apply:

- byte-identical imported assets and unusually large imported assets
- frequent trigger/Galaxy timer patterns and unit-group scan hotspots
- semantic duplicate catalog objects that match after ignoring editor-only text,
  icon, tooltip, and category fields

These checks follow the same general direction as community SC2 tooling such as
Talv's Plaxtony and sc2-galaxy-toolkit: Galaxy, Trigger XML, Layout XML, and
GameData should be analyzed as structured SC2 data instead of treated only as
plain text. This project does not vendor that TypeScript code.

### Rename To Standard

Preview and apply rename plans for unit-family style data, including archive
mode. This remains review-first because many maps use custom naming rules.

### Optimization Wizard

Build one batch plan, review every step, apply changes, refresh analysis, then
close the wizard. One unsafe unused-chain row no longer cancels the whole apply;
it is skipped and reported while the rest of the selected safe work continues.

Very large Data Collection plans remain available for inspection but are not
preselected. This prevents a broad family-detection result from adding tens of
thousands of records without explicit review; the remaining safe cleanup steps
still run for large extension mods and asset packs.

### Closed Project / Aggressive standardization

Settings includes an explicit **Closed Project / Aggressive standardization**
option. It enables public ID/import changes for `.SC2Mod` and `.SC2Campaign`
sources, uses higher reviewed batch limits, and allows the rename-first,
Patterns-then-Records workflow. Enable it only when every consuming map/mod is
owned and will be updated together. The command-line equivalent is
`SC2OptimizeMaps --closed-project <archive.SC2Mod>`.

When archive optimization rewrites an MPQ-backed `.SC2Map` or `.SC2Mod`, the
archive writer also attempts a best-effort compact/repack after replacements or
deletions. If StormLib cannot safely compact a legacy archive, the already
verified rewrite is kept instead of failing a valid cleanup.

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
$env:QT_ROOT = 'C:\Qt\6.8.2\msvc2022_64'
.\scripts\dev.ps1 -Action Test -Configuration Debug
.\scripts\dev.ps1 -Action Run -Configuration Debug
```

Release executable:

`build/windows-msvc/Release/SC2DataHelper.exe`

`scripts/dev.ps1` is also the recommended entry point for VS Code and coding
agents. It uses the checked-in CMake preset instead of paths specific to one
computer. Install CMake, Visual Studio 2022 with C++ support, and a Qt kit;
set `QT_ROOT` to that kit before running it.

## Notes

- Always test the optimized copy in StarCraft II Editor
- XML analysis cannot prove every binary/runtime reference inside archives
- This is a community project and is not affiliated with Blizzard Entertainment
