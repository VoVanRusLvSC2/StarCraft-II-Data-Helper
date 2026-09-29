# Native GUI types and catalog-scoped CLI verification

Runtime now ships an immutable NativeLib type snapshot from the supplied build 97563 corpus. Its source is nativelib.triggerlib; exact source SHA256 cbb2302825dce57ff9bb8efd44f9b0ed5cb056eee00265e5b711b0824d872396. The generated resource has 6554 ParamDefs and 3196 FunctionDefs, 774733 bytes, SHA256 c58ea0734694b55153dba68f96d80801fc9b717ad758e23fb2a96bd7d571fdb5. generate_gui_type_bindings.py is offline, disables external resolution, rejects DTD/root namespaces/conflicting definitions, preserves library identities and missing type information, and emits canonical CRLF bytes on all hosts. Running it against the supplied source reproduced the bundled resource byte-for-byte. Runtime does not invoke Python.

GuiTypeBindings.h verifies the artifact hash, source hash, format and build before admitting any types. Immutable cached generations refresh when bytes change. Registry imports only definitions actually referenced by project Params/FunctionCalls. Local declarations conflicting with the snapshot remain ambiguous; they do not silently replace it. Native provenance is attached to supported reference explanations. Actual UnitCreate FunctionDef Ntve|6C39A0DF and its Unit gamelink ParamDef Ntve|EF0CF6FF were independently read from the supplied XML before implementation.

The shared schema/type fingerprint now also contains active native binding bytes. Analysis start/end and existing destructive preflights therefore detect a changed GUI type resource. A changed resource is rejected as type evidence after a new analysis too; invalid/missing stock data remains unresolved instead of producing an empty safe graph. Project definitions can still resolve without stock data. Same ParamDef ID in different libraries is independent.

Special catalog-facing GUI types must not be treated as ordinary display values. catalogentry, abilcmd and soundlink now protect known targets; composite/dynamic values remain Unknown. Unresolved sameas/sameasparent/anyvariable, actor messages and user instance/field contexts also retain conservative protection. This is not a compiler for their full grammar. The test checks real protection plus an independently Safe Effect.

CLI --delete-unused had a separate verified defect: removing Actor SharedAudit succeeded in the core, but a final ID-only check rejected the retained Unit SharedAudit. OptimizeFolder now checks catalog+ID and scoped references. JSON adds unusedObjects with catalog, ID, element, relative source and declaration location; unusedIds remains for compatibility. The real CLI regression preserves the Unit and GUI bytes while removing Actor and an independent Effect. This scoped reread is not the requested full independent semantic verifier; that remains pending.

Debug CLI exposed an additional resource lookup gap when its working directory is a temporary fixture rather than the project. CatalogLinkSchema now also checks the common CMake runtime resources directory adjacent to Debug/Release, after existing overrides. This keeps explicit incompatible-index tests meaningful while allowing tool execution outside the source tree.

## Before/after evidence

- gui-native-before.txt: real Ntve parameter produced zero Strong typed references.
- gui-native-conflict-before.txt: a conflicting definition protected only one of the two possible catalogs.
- gui-native-special-before.txt: catalogentry was proposed Safe despite a GUI parameter using it.
- gui-cli-identity-before.txt: CLI rejected retained Unit identity after deleting the independent Actor identity.
- gui-cli-debug-resource-before.txt: Release passed but Debug CLI did not propose deletion because schemas were not found from its temporary working directory.
- Regression suite includes guiNativeFunctionParameterUsesShippedTypes, guiNativeConflictingProjectDefinitionRemainsUnknown, guiNativeBindingChangeRejectsStaleApply, guiLibraryScopedParameterDefinitionsStaySeparate, guiCatalogFacingSpecialTypesRemainProtected and optimizationCliKeepsOtherCatalogIdentity.
- Five offline GUI binding generator tests are registered in CTest alongside the five existing structural index tests.

## Readiness limits

Native definitions apply to the supplied build-97563 snapshot, not all future Editor versions. Full FunctionCall expression/call graph, catalogentry dependent contexts, composite ability/sound/actor/user grammars and layer overrides are not claimed. Native GUI typing is used by analysis, rename and GUI merge paths, but the overall task remains incomplete: dependency/effective-values loading, default/builtin token rules, all XML/Objects mutation carriers, cross-catalog merge redirects, concurrent dependency/settings guards and the independent semantic verifier need further work. Editor acceptance remains NOT_RUN. Final build/test/portable results are appended only after their execution completes.

## Final verification - 2026-09-28

Final Debug and Release each passed 5/5 CTest. Core: 142 passed, 0 failed, 4 skipped in each; Debug 221.780 s (229.73 s total), Release 74.951 s (84.03 s total). Both used the copied Mercs Episode 2 archive; its archive tests actually ran. The unavailable Gargantua/ZombieWorld/M3 fixtures remain skipped, not PASS. Logs: core-tests-*-gui-native-types.txt and gui-native-verified-*.txt. The earlier failed Debug run and short resource-fix regression are retained as evidence; no assertion was weakened.

The fresh app/probe/CLI were tested in hardening-stage/gui/native-preflight outside the source project with PATH limited to Windows/System32. App exit 0 and seven layout profiles PASS, zero failures; offscreen OpenGL context false is not GPU acceptance. Probe: Complete within the fixture scope, 3 objects, 2 Safe candidates, no Unknown. The Safe NativeAudit is Actor, not the referenced Unit; the CLI scoped report proves the distinction. Preview wrote no deletions. Real --delete-unused on a separate fixture copy removed Actor and independent Effect, retained Unit and exact GUI bytes, returned success; the original fixture hash stayed unchanged. This is a static fixture test, not an Editor/runtime claim.

Four sequential read-only benchmark processes ran after builds/tests finished: baseline small/large then current small/large, same archive copies and method. Baseline source is commit 3301dd13bc7f1fb10f205867de05257bfd4a17d6. OS caches were not reset; single runs do not establish stable acceleration.

| Input copy | Baseline/current seconds | Peak MiB baseline/current | Objects | References | Unknown candidates | Safe candidates |
|---|---:|---:|---:|---:|---:|---:|
| Mercs Episode 2 | 2.006 / 2.728 | 46.8 / 158.3 | 863 / 863 | 7842 / 13120 | 0 / 853 | 345 / 0 |
| City of Tempest | 38.384 / 49.448 | 237.5 / 465.0 | 12855 / 12856 | 96546 / 148155 | 0 / 12805 | 7121 / 0 |

Files remain 75 and 527 respectively. The additional large-map object is an ID-less default declaration. Current maps are Partial because declared dependencies remain unresolved, with lexical/semantic limitations retained in raw reasons. Zero Safe does not prove all objects are used. Additional typed schema, descendant-context references, lexical coverage and provenance cost memory/time; in this run large-map duration is about 29% above baseline and peak memory nearly doubles. Performance is an open limitation; next profiling must separate schema loading, resolver, reference indexing and cleanup instead of attributing the entire difference to NativeLib. The native snapshot imports only referenced definitions and caches parsed immutable generations.

Archive SHA256 remained equal to originals after all tests/benchmarks: Mercs 22d2dc6d6892bff006e0cad9589174168c54342b773f65a0d6ea4b0fabc97729; City 7c52a0da5a2e0943ee19a533b76606ee50880aec18bec4e72a3593a65780d74c. Raw JSONs are benchmark/gui-native-*.json. Duration excludes the extra index build for record counts; peak includes it, identically for baseline/current.

Next concrete task is to replace remaining broad XML/merge mutation heuristics with the shared typed reference contract, including ambiguous IDs across catalogs, case-policy consistency, unknown Galaxy/XML consumers and independent output reading. Then implement project components/includes, dependency layers, class defaults and confirmed effective values, plus the remaining full-plan transaction/fidelity audit. Full assignment remains active, Editor NOT_RUN.

## Portable artifact

The new package is dist/SC2DataHelper-3.0-beta3-gui-types-20260928 and the matching ZIP (71543259 bytes). SHA256: 1f4e3dda3f232e757ea81eb4651820bd69c671b7a077e7b96786fc10a7911b33. All 317 manifest files, ZIP CRC/content hashes, UTF-8 Russian README and four binaries against the final build were independently checked. Packaged probe ran with only Windows/System32 on PATH: Complete fixture scope, 3 objects, 2 Safe, no Unknown. The package is a checkpoint of the verified native/GUI/CLI subset; full assignment INCOMPLETE, Editor NOT_RUN are explicit in the manifest and README. Main documentation includes this post-packaging verification; packaged docs capture the pre-zip checkpoint to avoid a self-referential artifact hash.
