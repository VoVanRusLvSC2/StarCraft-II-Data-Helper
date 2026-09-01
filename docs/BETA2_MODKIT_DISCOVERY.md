# Beta 2 Modkit and SC2 preview discovery

Audit date: 2026-09-02. Diagnostics are stored under
`target/diag/beta2-real-maps/` and are intentionally ignored by Git.

## Decision table

| Candidate | Canonical source | License/source status | Formats and useful parts | Qt/Windows cost and supply-chain risk | Decision |
|---|---|---|---|---|---|
| Installed `talv.sc2galaxy` 1.10.5 / plaxtony 1.10.5 | https://github.com/Talv/vscode-sc2-galaxy | MIT; locally installed source/bundle was inspected | Galaxy language service and legacy dependency/catalog indexing; no `Preview SC2Map` renderer | Node/VS Code runtime is unsuitable as a production dependency; old stable build predates the requested viewer | Reference only |
| Modkit 2.1.0 official Marketplace VSIX | https://marketplace.visualstudio.com/items?itemName=talv.sc2galaxy | Official VSIX is available, but `package.json` has no license and the published repository says that source is not published | Commands `modkit.previewMap` and `modkit.previewMap3d`, `.SC2Map`/`.s2ma` custom editor, MPQ/CASC browsing, terrain and 3D modules are present in the minified bundle | 47.8 MB opaque executable bundle, VS Code/webview/Node coupling, no auditable source or reusable viewer API | Reject copying; behavioral/format reference only |
| Current `sc2-galaxy-toolkit` 2.0.0 workspace | https://github.com/sc2-arcade-watcher/sc2-galaxy-toolkit | Packages declare MIT; `vscode-sc2-galaxy/LICENSE` is MIT, copyright Talv | Galaxy, triggers, GameData/XML, layouts, extracted-component dependency resolution; no map renderer, MPQ reader, terrain decoder, DDS/TGA or M3 viewer in the public workspace | TypeScript/Node adapter would add a runtime and does not provide the missing renderer | Reference only |
| StarCraft II Editor | Locally installed Blizzard Editor | Proprietary external executable | Authoritative visual/load/trigger oracle | GUI ownership and unattended readiness are difficult to prove; never bundle assets | External oracle only |
| `sc2-file-format-docs` | https://github.com/sc2-arcade-watcher/sc2-file-format-docs | Documentation-only clean-room input; commit pinned in diagnostics | Verified `MapInfo`, `t3Terrain.xml`, `t3HeightMap` dimensions, offsets, scale, cell layout and Y ordering | Reverse-engineered specifications must be validated against real maps and Editor | Use as clean-room format specification |
| Project readers + Qt | This repository | Project license | StormLib MPQ reader, Objects, exact Regions, MapInfo and terrain metadata, Qt TGA decoder and cached canvas | Native implementation; no VS Code or network runtime dependency | Use |
| M3 viewers/loaders | Not selected for Beta 2 Level A | Various; no audited dependency was needed | Model-only layer cannot provide a complete map preview | Additional asset resolution and copyright risk | Defer Level C |

## Exact inspected artifacts

- Local stable extension: `C:/Users/Vladimir/.vscode/extensions/talv.sc2galaxy-1.10.5`.
- Marketplace extension: `talv.sc2galaxy` 2.1.0, downloaded without installing or replacing the user's extension.
- VSIX SHA-256: `50FC0D205134B926990C1AFF1EDD9A0C37E511E7C9D9B0EBFA7C53AC1834052B`.
- VSIX manifest declares `modkit.previewMap`, `modkit.previewMap3d`, an SC2 map custom editor, and repository `sc2-arcade-watcher/sc2-modkit`.
- Current Modkit issue repository commit: `ccbcca8d6ad51ccbf3711c1786a06dad3a6fdea3` (2026-08-04). Its README explicitly states that the extension source is not published.
- Current public toolkit commit: `95d1ff82b8e89fb0078c4b8e5e6622271b927b94` (2026-07-01).
- Format documentation commit: `81426e97401d6583ff4bce7d2d0d4cbd99d195c1`.

The VSIX source map exposes module names such as `map-info.ts`,
`terrain-binary.ts`, `height-map.ts`, `map-preview.ts`, `map-preview-3d.ts`,
`scene-3d`, MPQ and CASC readers. It contains no embedded source text. The
bundle is therefore evidence that the viewer exists, but not licensed source
that can safely be copied into this project.

## Integrated and clean-room parts

No code was copied from the opaque Modkit 2.1.0 bundle. The public toolkit is
used as a pinned behavioral/data-model reference, not as a runtime dependency.
The native Qt implementation uses the project's StormLib archive inventory,
exact `Regions` and `Objects` readers, and a new clean-room decoder for:

- `t3Terrain.xml` height-map dimensions, origin and scale;
- `t3HeightMap` strict `HMAP` version 101 header and six-byte cells;
- `MapInfo` width/height fallback, explicitly marked approximate;
- `Minimap.tga` through Qt's image stack;
- aspect-preserving world-to-screen mapping, Y inversion and letterboxing.

The canvas shows a real minimap or terrain raster, or the explicit message
`MAP PREVIEW UNAVAILABLE: <reason>`. It never substitutes an object bounding
box as a proven map background. Exact Region classification remains in world
coordinates even when only approximate background alignment is available.

## Remaining limitations

- The production map viewer is clean-room and format-compatible, not a direct
  port of Modkit's unpublished renderer. This blocks any claim of literal
  Modkit renderer integration.
- Level C M3/material rendering is not implemented.
- The canvas uses a `QOpenGLWidget` viewport with cached static layers. The
  automated layout matrix records a valid context in automatic mode and with
  `QT_OPENGL=software`; real-map first-paint timing remains to be measured.
- Visual parity still requires comparison with the Editor on real maps.
