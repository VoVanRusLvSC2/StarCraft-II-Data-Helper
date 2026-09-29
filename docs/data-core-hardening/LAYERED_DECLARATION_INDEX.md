# Layered declaration index and unresolved precedence (2026-09-28)

After the structural catalog schema is refreshed, the active model now indexes all named declarations from the local source and active dependency GameData layers by catalog/ID. Each reference retains the original element class, exact ID spelling, source XML path, archive layer index and node index. Exact-class default declarations without an ID have a separate index. The index retains all collisions; its stable source sort is for reporting and is **not** runtime override precedence.

The local resolver still computes its established closed-chain subset, then invalidates `known` effective values when a dependency declares the same catalog/ID, when an inherited local parent has such a collision, or when the object's exact class has a dependency default. Raw spelling and source provenance remain available. Resolution diagnostics and Unknown catalog coverage block false safe cleanup or deletion. An unrelated catalog with the same ID stays separate. A `Shared`/`SHARED` case variant remains one indexed collision under the application's existing case-folded identity policy, with both raw spellings retained; definitive Editor case semantics remain unproven.

The synthetic MPQ regression checks three colliding CUnit declarations across map and two mods, a separate CEffect with the same ID, two class defaults, propagation through a CButton parent without a dependency default, and known=false for affected local scalar values. The earlier unique local scalar/default tests still pass.

On the copied City map with two copied dependency mods, the read-only probe found 14,122 catalog/ID groups, 80 groups with multiple declarations, 74 spanning more than one source layer, and 6 groups with multiple raw ID spellings. No class-default collision was observed in the loaded subset. The map remains Partial because four direct/transitive sources are absent; zero Safe unused candidates. The probe took 40,362 ms and peaked at 512 MiB, a single run that does not establish a performance trend. Evidence: `benchmark-layered-index-city.json`.

This index does not determine which declaration wins, combine fields, resolve cross-layer parents/tokens or assign runtime array/index/removed semantics. Editor/gameplay acceptance is NOT_RUN.

Final Release: 5/5 CTest, 165 core passed / 0 failed / 4 unavailable fixture skips, 98.99 s total. Final Debug: 5/5 CTest, 165 core passed / 0 failed / 4 unavailable fixture skips, 284.23 s total.

Current portable checkpoint: `dist/SC2DataHelper-3.0-beta3-layered-index-20260928.zip`, SHA-256 `c52ceeb66ba08ca59461c25e6fdc96aa3cb57e5523e16f523a264659d81d9c8c`, 194 verified ZIP entries/resources. Outside-source layout smoke generated 14 PNGs; read-only Mercs map preview returned ready=true and source_unchanged=true with 75 archive entries. Offscreen OpenGL unavailable; Editor/gameplay acceptance NOT_RUN.
