# Performance and coverage - 2026-09-27

Baseline is exact commit 3301dd13bc7f1fb10f205867de05257bfd4a17d6 restored using git archive. Baseline core is unchanged; the identical read-only probe and CMake target were added. Both Release/MSVC 19.44/Qt 6.8.2. Identical copies were used; original/copy hashes still match (benchmark-inputs.json).

Final runs were sequential after core tests/builds: baseline small/large, then after small/large. One cold process per input, Windows file cache not reset. No statistical speed promise.

| Copy | Seconds before -> after | Peak MiB | Files | Declarations | Reference records | Unknown candidates | Safe candidates |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mercs Episode 2 (3.84 MB) | 2.342 -> 3.098 | 45.1 -> 158.1 | 75 -> 75 | 863 -> 863 | 7842 -> 11970 | 0 -> 847 | 345 -> 0 |
| City of Tempest (39.81 MB) | 41.701 -> 41.134 | 237.4 -> 482.3 | 527 -> 527 | 12855 -> 12856 | 96546 -> 146979 | 0 -> 12805 | 7121 -> 0 |

Large-map timing is comparable (about 1.4% difference); small-map overhead is about 0.76 s / 32%. Memory increased substantially due to structural runtime schema, inherited typed edges/provenance and conservative coverage records. This remains a limitation. Repeated XML parses across token/reference passes and repeated schema-scope walks per class were removed. Preliminary 58.87 s after-large ran concurrently with tests; its difference from final 41.13 s cannot all be attributed to the cache change.

Duration includes MPQ loading/reading, GameData extraction, source hash, finalizeAnalysisResult and reports. An extra UnifiedReferenceIndex used to count records is built after duration. Windows GetProcessMemoryInfo peak is read after that pass and includes the whole process. Both engines use the same method. Records include inherited and synthetic blocking references, not only proven runtime links. One additional default without ID accounts for the large declaration count change.

Before: Complete, Unknown 0 on both maps. After: Partial because declared dependency layers are not loaded. Small declares Mercs Mod LOTV; large declares Left 2 Die, Protis and stuff, Spectre Hero Pack and Swarm Story. Missing/ambiguous local parents, unknown/builtin tokens and dynamic UnitCreate also contribute. Raw final JSON contains grouped reasons and incomplete_sources.

The new result fixes false completeness; it does not prove that every former Safe was actually used. Declared unresolved dependencies block destructive apply globally. Scoped Unknown for supported grammars leaves independent fixture work available. No Editor/runtime or full effective dependency/array model acceptance. Large memory increase is explicitly unresolved.

2026-09-28 raw-array checkpoint: the same copied Mercs and City inputs measured
2,907/42,150 ms and 166,014,976/576,417,792 peak bytes. They contained
5,186/54,354 own repeated-field declarations; both remained Partial with
zero Safe and 853/12,805 Unknown. The first source model copied parent arrays
to descendants and measured 581,058,560 peak bytes on City; storing each
source once measured about 576.4 MB in two repeats. This reduced peak memory
by about 4.5 MB in the controlled comparison. Relative to the 2026-09-27
baseline above, many semantic stages changed, so the current memory delta
cannot be attributed to arrays alone. See RAW_ARRAY_DECLARATIONS.md.
