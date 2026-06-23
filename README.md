# StarCraft II Data Helper

**StarCraft II Data Helper** is a Windows desktop tool for analyzing and optimizing
StarCraft II XML data in maps, mods, and component folders. It helps SC2 modders
clean catalog data, inspect references, safely merge duplicates, and maintain
Data Collections without editing every XML file by hand.

[Download the latest version from Releases](https://github.com/VoVanRusLvSC2/StarCraft-II-Data-Helper/releases)

> The project is under active development. Always keep a copy of your map or mod
> and review the generated preview before applying changes.

## Features

### Remove unused data objects

Find catalog objects that have no incoming XML, script, or text references.
Objects are never deleted automatically: every candidate must be selected and
previewed by the user. Whitelisted, root, runtime-protected, and referenced
objects are blocked from deletion.

### Find and merge exact duplicates

Detect objects of the same type with identical normalized XML bodies. Choose
which object to keep, preview every affected reference, redirect references to
the kept ID, and then remove the duplicate. Replacement is token-aware, so an ID
inside a longer name is not changed accidentally.

### Rename data objects *(beta)*

Build a preview for standardizing related unit-family IDs and update supported
references. Rename operations remain review-first because custom maps can use
project-specific naming and runtime references.

### Build and update Data Collections

Create `CDataCollectionUnit` entries or add missing records to an existing Data
Collection. Existing collections, records, custom attributes, metadata, and
unrelated XML nodes are preserved. The tool also creates or updates the archive
`(listfile)` entry when required.

### Analysis and inspection

- Browse SC2 catalog objects and their properties.
- Inspect incoming and outgoing dependencies.
- Visualize the reference graph.
- View complete source XML with highlighting.
- Review warnings and optimization reports per file.

## Safe optimization workflow

1. Open an `.SC2Map`, `.SC2Mod`, XML file, or extracted component folder.
2. Run **Analyze**.
3. Open **Optimization** and build the preview.
4. Review and select changes in all five steps.
5. Apply the plan and save only after checking the summary.

Destructive operations are preview-first and backup-first. Folder/XML mode can
apply verified changes. Archive operations are limited when binary references
cannot be checked safely.

## Building from source

Requirements:

- Windows 10 or Windows 11
- CMake 3.24 or newer
- Qt 6.5 or newer with Widgets, Test, and Concurrent
- A C++20 compiler such as Visual Studio 2022

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The executable is generated at `build/Release/SC2DataHelper.exe` when using the
Visual Studio generator.

## Important notes

- Test the optimized copy in the StarCraft II Editor before publishing it.
- Script-generated and binary references cannot always be proven from XML alone.
- This is an independent community project and is not affiliated with or
  endorsed by Blizzard Entertainment.

## Contributing

Bug reports and reproducible SC2 XML examples are welcome through
[GitHub Issues](https://github.com/VoVanRusLvSC2/StarCraft-II-Data-Helper/issues).
