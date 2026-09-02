# StarCraft-II-Data-Helper 3.0 Beta 2 — real-map validation report

Evidence date: 2026-09-02. Release state: **BETA 2 BLOCKED**.

This is a verification candidate, not Stable/Final. No GitHub release was
created and nothing was pushed. Every source `.SC2Map`/`.SC2Mod` was treated
as read-only; all generated archives and screenshots remain below
`target/diag/beta2-real-maps/` and are ignored by Git.

## Build and commit range

- Baseline: `76124d7 feat: select exact map regions for decor streaming`.
- Final code commit: `f524c73 fix: block incomplete dependency inventories`.
- Implementation commits in this phase: `de2a79c`, `216c6e6`, `99f8525`,
  `cd8f945`, `9b06f23`, `ef0cf4a`, `e34d8d4`, `f524c73`.
- Windows MSVC Release, CMake, Qt 6.8.2, StormLib; product label
  `3.0 Beta 2`.
- Debug suite after Region changes: 3/3 PASS. Final Release suite: 3/3 PASS.
- Full final corpus run duration: 00:22:46. Autonomous goal elapsed at final
  evidence collection: about 03:27, within the 10-hour limit.

Primary machine-readable evidence:

- `target/diag/beta2-real-maps/final-run/manifest.json`
- `target/diag/beta2-real-maps/final-run/aggregate-report-audited.json`
- `target/diag/beta2-real-maps/final-run/source-hash-final-audit.json`
- `target/diag/beta2-real-maps/final-run/editor-oracle/editor-oracle-audited.json`
- `target/diag/beta2-real-maps/real-map-preview/real-map-preview.json`
- `target/diag/beta2-real-maps/portable-smoke-f524c73/portable-smoke.json`

## Outcome and safety proof

The dynamic scan found 22 corpus documents; the exact required mission made
23 processed documents. Final outcomes were 17 `OPTIMIZED_COPY_VERIFIED`, five
`NO_SAFE_GAIN`, and one `BLOCKED_ARCHIVE_OPEN`. All 23 before/after source
SHA-256 comparisons matched. The one unreadable archive is
`StarParty.SC2Map` (StormLib error 10009); its dependency state is
`BLOCKED_INCOMPLETE_DEPENDENCY_INVENTORY`, never `RESOLVED`.

All 17 optimized copies reopened through a fresh backend and passed fresh XML,
Objects, Regions, Galaxy/reference analysis. `logical_entry_preservation` is
PASS and `unexpected_entry_changes` is empty for all 17. Non-replacement
payload bytes remained identical; only the planned Objects/MapScript/runtime
entries and necessary MPQ `(listfile)`/`(attributes)` internals changed.

The required mission source SHA-256 is
`d15a56889e07ce40fd06e80389968c75283697523270c3f9054ab69953e2b921`
before and after. Its copy SHA-256 is
`2c2ce582176475a8b578dba2fa82584dc39fe89808777725d60197dcd1b1629b`.
It removed 3,940 proven decor placements, added five public Galaxy functions,
preserved outside-scope entries, and passed fresh structural and semantic
verification.

## Per-document matrix

`Saved` may be negative for decor streaming because generated Galaxy can be
larger than removed Objects XML. This is not compression-only mode.

| Map | Source bytes | Output bytes | Saved | Analysis | Semantic verify | Editor | Original unchanged |
|---|---:|---:|---:|---|---|---|---|
| City of Tempest KSP Production | 39,809,791 | 39,812,304 | -2,513 | COMPLETE | PASS | FAIL_EDITOR_ALERT | YES |
| Cold expedition | 115,514,890 | 115,499,403 | 15,487 | COMPLETE | PASS | NOT_PROVEN_DOCUMENT_READY | YES |
| Ice Baneling Escape Arctic Adventures | 5,843,168 | 5,845,811 | -2,643 | COMPLETE | PASS | FAIL_EDITOR_ALERT | YES |
| Merces Episode 3 | 10,301,020 | 10,306,277 | -5,257 | COMPLETE | PASS | FAIL_EDITOR_ALERT | YES |
| Mercs Episode 1 | 5,772,798 | 5,776,086 | -3,288 | COMPLETE | PASS | FAIL_EDITOR_ALERT | YES |
| Mercs Episode 2 | 3,840,869 | 3,843,867 | -2,998 | COMPLETE | PASS | FAIL_EDITOR_ALERT | YES |
| NydusMod (2)…SC2Mod | 538,903 | — | 0 | COMPLETE / NO_SAFE_GAIN | N/A | NOT_PROVEN_DOCUMENT_READY (baseline copy) | YES |
| Project [DELETED] | 118,368,686 | 118,368,275 | 411 | COMPLETE | PASS | NOT_PROVEN_DOCUMENT_READY | YES |
| Rainbow Six Арена сюнь - 20.10.2021 | 122,526,304 | 122,527,828 | -1,524 | COMPLETE | PASS | NOT_PROVEN_DOCUMENT_READY | YES |
| Rainbow Six Арена сюнь - Последняя Зима | 156,699,931 | 156,700,726 | -795 | COMPLETE | PASS | NOT_PROVEN_DOCUMENT_READY | YES |
| Rainbow Six Арена сюнь Мод | 13,234,886 | — | 0 | COMPLETE / NO_SAFE_GAIN | N/A | NOT RUN | YES |
| Rainbow Six Арена сюнь | 174,443,968 | 174,335,559 | 108,409 | COMPLETE | PASS | NOT_PROVEN_DOCUMENT_READY | YES |
| Rainbow Six Сюнь Арена | 9,007,989 | 8,765,722 | 242,267 | COMPLETE | PASS | NOT RUN | YES |
| Rainbow_Six_Arena-16.01.2019 | 156,699,935 | 156,700,730 | -795 | COMPLETE | PASS | NOT RUN | YES |
| STAR CRAFT Roleplay | 67,232,360 | 67,235,687 | -3,327 | COMPLETE | PASS | NOT RUN | YES |
| StarParty Mod.SC2Mod | 39,456,129 | — | 0 | COMPLETE / NO_SAFE_GAIN | N/A | NOT RUN | YES |
| StarParty.SC2Map | 4,860,680 | — | 0 | BLOCKED_ARCHIVE_OPEN | N/A | NOT RUN | YES |
| The Dark Story | 44,650,067 | — | 0 | COMPLETE / NO_SAFE_GAIN | N/A | NOT RUN | YES |
| Unitazophobia / Унитазофобия | 78,605,007 | 78,591,178 | 13,829 | COMPLETE | PASS | NOT RUN | YES |
| Victor's Reapers | 9,834,398 | 9,835,588 | -1,190 | COMPLETE | PASS | NOT RUN | YES |
| VictorMod.SC2Mod | 12,979,022 | — | 0 | COMPLETE / NO_SAFE_GAIN | N/A | NOT RUN | YES |
| Эпические Битвы с Боссами | 43,254,381 | 43,258,356 | -3,975 | COMPLETE | PASS | NOT RUN | YES |
| Required mission | 5,605,941 | 5,595,285 | 10,656 | COMPLETE | PASS | NOT_PROVEN_DOCUMENT_READY | YES |

## Editor oracle

The prepared matrix contained 24 candidates and covered the required mission,
two small, two medium, two large, complex/Cyrillic names, a local-mod case and
an `.SC2Mod`. Twelve distinct documents were launched only from diagnostic
copies. No pre-existing Editor process was controlled.

The first detector revision was deliberately invalidated because it treated a
generic responsive window as document-ready and missed singular Russian
`Ошибка`/`Предупреждение`. The audited evidence and hardened runner classify:

- five `FAIL_EDITOR_ALERT` (missing/resource/dependency/error dialogs);
- seven `NOT_PROVEN_DOCUMENT_READY` (generic Terrain/Messages or
  `[Безымянная карта]` windows);
- zero Editor PASS;
- trigger validation `NOT_PROVEN` and runtime `NOT_RUN` for every document.

The hardened required-mission smoke reproduced
`Поверхность - [Безымянная карта] - Редактор StarCraft II`, therefore the
required mission Editor gate is not met. Internal semantic PASS is not used as
a substitute.

## Maximum compatible compression

Compression-only is a separate UI/worker operation and never removes assets.
Preflight checks source/output identity, free space, named versus physical MPQ
inventory, unsupported patch/delete/signature/unknown flags and source
staleness. It compacts a temporary archive, freshly reopens it, compares every
logical entry hash, commits atomically only when smaller, and removes staging
files on cancel/failure/no gain.

Of 22 readable documents, all logical-entry comparisons passed. Only two
became smaller:

| Map | Source | Compressed | Saved | Logical equality | Editor |
|---|---:|---:|---:|---|---|
| Merces Episode 3 | 10,301,020 | 10,300,783 | 237 | PASS | NOT RUN |
| Rainbow Six Сюнь Арена | 9,007,989 | 8,876,876 | 131,113 | PASS | NOT RUN |

The other 20 readable archives returned `NO_COMPATIBLE_SIZE_GAIN` and created
no larger output. The mission was one of these. The implementation is verified
MPQ compaction, not a proven per-entry minimum-codec search, and neither output
reached Editor-ready acceptance. Consequently the word `Compatible` remains a
candidate-mode label, not a completed compatibility claim, and the compression
acceptance gate (three gains plus Editor proof) is BLOCKED.

## Viewer and Modkit audit

The full discovery report is `docs/BETA2_MODKIT_DISCOVERY.md`.
The installed Talv/plaxtony 1.10.5 was inspected. Official Marketplace Modkit
2.1.0 was downloaded without replacing it; VSIX SHA-256 is
`50FC0D205134B926990C1AFF1EDD9A0C37E511E7C9D9B0EBFA7C53AC1834052B`.
Its bundle contains the 2D/3D map commands and terrain modules, but the current
upstream states source is unpublished and the VSIX declares no reusable
license. No opaque code was copied. Public toolkit commit
`95d1ff82b8e89fb0078c4b8e5e6622271b927b94` is MIT but has no map renderer.

The licensed clean-room fallback uses StormLib plus real `Minimap.tga`,
`t3Terrain.xml`, strict HMAP v101 `t3HeightMap`, MapInfo fallback, Objects and
Regions. The QOpenGLWidget canvas has cached layers and software fallback,
aspect-preserving bounds, Y inversion, letterboxing, ruler/grid, pan/zoom/fit,
object toggles, hover/click selection and exact circle/rect/polygon/diamond/
composite overlays. Unknown forms remain fail-closed and show raw parameters.

Required-mission preview evidence: 98 entries, 4,853 positioned objects, 58
Regions, exact 0..256 terrain bounds, valid/composited OpenGL framebuffer,
source hash unchanged, and first ready paint 51,096 ms. This is slow but was
off the UI thread. Visual parity with Editor is not proven because the Editor
did not load the requested document; M3/material preview remains optional and
unimplemented.

## Responsive layout, background work and public API

The final portable passed all seven automated profiles with 20 interactive
controls inside the accessible viewport: 1280x720@100%, 1366x768@100/125% and
1920x1080@100/150/200%, across English/Russian. Each profile saved overview and
action screenshots. Automatic OpenGL and `QT_OPENGL=software` both produced a
valid context. Archive scan, XML/reference analysis, terrain rendering,
compression and verification use cancellable workers; canvas painting/model
updates remain on the UI thread.

Repository search returned zero public occurrences of `Actors created per game
tick` or `Action per tick` under source/tests/translations/resources. Generated
public Galaxy functions take no per-tick parameter; the internal batch limit is
fixed implementation detail.

Measured baseline/fresh analysis examples were 181 ms for the 0.54 MB mod,
4,460/4,334 ms for the 39.8 MB City map, and 4,028/3,887 ms for the 174.4 MB
largest map. Other 156.7 MB maps took up to 14,802/13,871 ms. Peak working set,
longest UI blocked interval, and separately split compression/verification time
were not reliably instrumented and are reported `NOT MEASURED`, not PASS.

## Failure injection status

Synthetic PASS coverage includes corrupt MPQ, malformed/unknown Region,
malformed terrain height data, stale source during analysis and before commit,
strong-reference preflight, existing output preservation, simulated
insufficient space, cancellation before compression and during verification,
larger/no-gain output cleanup, logical mismatch prevention, and software
OpenGL fallback. These tests verify unchanged source and no committed partial
output where applicable.

Dedicated injections for a read-only output ACL, worker exception, explicit
Editor-rejects-internally-valid fixture, invalid generated Galaxy, and a
standalone truncated Objects fixture were not all independently exercised in
this run. They remain `NOT RUN`; this is another reason not to call the build
Stable/Final.

## Packaging

Final portable folder:
`target/diag/beta2-real-maps/portable/StarCraft-II-Data-Helper-3.0-Beta-2-f524c73`.

Final ZIP:
`StarCraft-II-Data-Helper-3.0-Beta-2-f524c73-win64.zip`, 49,088,297 bytes,
SHA-256 `9C29C480089F6984BEF379B4ACBEAF529402D60150B5F6FD2E07CA9AE537698C`.
The GUI executable SHA-256 is
`3E38FD24F64446E22C9D3BD514244CC01C32630931C91CB1F4380ED2DB4C1993`.

Clean-PATH smoke (`C:\Windows\System32;C:\Windows`) exited 0 with all seven
profiles PASS. Qt6Network, Qt6OpenGL, Qt6OpenGLWidgets and `opengl32sw.dll` are
present. Installer build/smoke is `BLOCKED_TOOL_UNAVAILABLE`: Inno Setup
`ISCC.exe` is not installed.

## Feature gates

| Feature | Implemented | Synthetic tested | Real-map tested | Editor tested | Limitations |
|---|---|---|---|---|---|
| Exact Region selection | YES | YES | YES, 17 copies | Attempted | Editor ready-state not proven |
| Objects decor removal | YES | YES | YES, 17 copies | Attempted | Some outputs grow due Galaxy API |
| Galaxy actor recreation | YES | YES | YES, fresh parse | Attempted | Trigger compile/runtime not proven |
| Outside-scope preservation | YES | YES | PASS, all 17 | Attempted | Editor blocked/not ready |
| Maximum compatible compression | PARTIAL | YES | 22 readable; 2 gains | NOT PROVEN | Compaction only; three-gain gate failed |
| Map Canvas | YES, Level A clean-room | YES | Required mission | Comparison BLOCKED | Modkit renderer source unpublished |
| Talv/plaxtony/SC2 Mod Tools data integration | AUDITED/REFERENCE | N/A | YES | N/A | No licensed renderer API to port |
| Responsive controls at required DPI | YES | 7/7 PASS | Portable smoke | N/A | Fullscreen is not separately scripted |
| Removed per-tick user setting | YES | Search/test PASS | Generated output checked | N/A | Internal fixed batching remains |
| GPU rendering/fallback | YES | PASS | Mission + portable | N/A | Editor GPU/runtime independent |
| Background analysis/UI refresh | YES | PASS | YES | N/A | Peak/UI-block telemetry incomplete |

## Exact manual checks left to the user

Use only the required mission copy under `final-run/documents`, never the
source. Verify that it opens as the intended named document, compile triggers,
launch the mission, compare the main and lower floors with the original, invoke
each generated Region create function, then clear/restore it. Confirm repeated
create is idempotent, clear/restore creates no duplicates, and gameplay units,
destructibles and trigger-referenced doodads remain present. Also inspect the
diamond Regions on a diamond-bearing map against the Editor overlay.

Until those checks, an Editor-ready acceptance run, three Editor-accepted
compression gains, and installer smoke succeed, the only honest state is:

`BETA 2 BLOCKED`.
