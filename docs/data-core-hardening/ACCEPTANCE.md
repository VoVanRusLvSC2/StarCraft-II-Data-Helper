# Acceptance - 2026-09-27

PASS applies only to the stated tested subset. PARTIAL/NOT_IMPLEMENTED are mandatory readiness limits. The whole assignment is not complete. All Editor/runtime acceptance: NOT_RUN. XSD is not proof of runtime array rules.

| ID | Status | Evidence / limitation |
|---|---|---|
| D01 | PASS | xmlTokenNoOpAndMutationPreserveData; exact no-op bytes, unknown extension retained |
| D02 | PASS | same fixture removes one neighbour; PI/comment/extension retained |
| D03 | PASS | xmlTokenDeclarationsAffectEquivalence |
| D04 | PARTIAL | structural wrapper/carrier independent XPath tests; index/removed effective semantics unavailable |
| D05 | PARTIAL | independent keys, scoped deletion/rename and CLI identity verification tested; cross-catalog merge mutations still need audit |
| D06 | PARTIAL | local same-catalog chain used by token tests; missing/ambiguous parent diagnosed; no dependency layers |
| D07 | NOT_IMPLEMENTED | declaredDependencyIsNotAssumedLoaded guards Partial; transitive loading/override absent |
| D08 | PARTIAL | catalogDefaultsAndNestedIdsAreSeparate; defaults indexed/protected, general default effective resolver absent |
| D09 | PARTIAL | XML preserves explicit carriers; no general effective empty/0/false model |
| D10 | PARTIAL | serialization retains repeats; array cleanup review-only; no runtime composition proof |
| D11 | NOT_IMPLEMENTED | XSD nested AST/index carrier retained; indexed struct effective model absent |
| D12 | PARTIAL | array/index/removed cleanup not Safe; no effective removed/reset semantics |
| D13 | PASS | tokenContextProtectsResolvedTargets in descendant context, conservative inherited refs |
| D14 | PARTIAL | token recursion/parent cycle bounded and scoped diagnostics; exhaustive negative fixture matrix absent |
| D15 | PASS | galaxyConcatenationProtectsTarget; limited UnitCreate literal/const/+ grammar |
| D16 | PASS | mutableGalaxyVariableDoesNotProveUnused; Unknown Unit and independent Safe Effect |
| D17 | PARTIAL | project FunctionDef/ParamDef registry drives analysis/rename/merge/batch; verified build-97563 NativeLib snapshot now shipped; expression/composite/dependent GUI contexts remain incomplete; see GUI_NATIVE_TYPES.md |
| D18 | PASS | actorEventChildCarriersAreNotBroken; attribute fixtures retained |
| D19 | PARTIAL | actorUnitBirthTokenCreatesUsageRoot; unknown token/wildcard retained; macros/commands not complete |
| D20 | PASS | unusedObjectChainsCoverFullCatalogGraphAndPlacementRoots; standaloneModExternalConsumersAreProtected |
| D21 | PARTIAL | unused chain graph/SCC reachability retained; full effective graph proof outside subset absent |
| D22 | PASS | catalogRemovalChecksIncomingDeclarationIdentity; partial chain negative tests |
| D23 | PARTIAL | deepCleanupRemovesRedundantInheritedXmlNodes; arrayEqualityIsNotRemovalProof; known scalar subset only |
| D24 | PASS | declaredDependencyIsNotAssumedLoaded; parseErrorBlocksFalseSafeUnused; oversizedGalaxySourceBlocksFalseSafe |
| D25 | PARTIAL | changedSourceRejectsStaleDestructiveApply; schemaCacheDetectsIncompatibleIndex; schema start/end/apply hash; layer/settings/concurrent commit guard incomplete |
| D26 | PASS | folderTransactionRollsBackOnValidationFailure; after-first-commit injected failure and original hash verification |
| D27 | PARTIAL | existing synthetic archive save-copy tests; real-copy read benchmark; equivalent folder/archive full semantic acceptance absent |
| D28 | PASS | 5 generator tests including deterministic exact bundled output; cache stale rejection |
| D29 | PARTIAL | Unknown/dynamic/binary preflight blocks; rename still legacy span/grammar limitations, not full acceptance |
| D30 | PASS | full existing suite retained; collection/template fixture contract corrected; CLI help/copy tests retained |

Final Debug and Release: 4/4 CTest PASS each, 127 core tests passed, 4 skipped. See STATUS.md, final logs and PERFORMANCE.md. Optional SC2DH_TEST_ARCHIVE points to a copy; three archive tests actually ran. Four missing audit/model fixtures remain skipped.

Reproduce with QT_ROOT=C:/Qt/6.8.2/msvc2022_64 and scripts/dev.ps1 -Action Test -Configuration Debug (or Release); Stage -Configuration Release deploys headless plugin and external resources. PowerShell execution policy required a child powershell -NoProfile -ExecutionPolicy Bypass invocation. Python/lxml index tests run offline; runtime does not need Python.

Editor packet: manual-editor/README_RU.md and three independently XSD-validated XML inputs. Record Editor version, dependencies, actual load/save/reopen/run logs. Inputs are not Editor golden outputs. See STATUS for mandatory incomplete mutation-span, layered/effective-values and coverage criteria.

Galaxy continuation: shared typed literal spans now drive analysis, rename and merge for supported native calls. Exact display/comment preservation, catalog-scoped rename, opaque string blocking and constant shadow/cycle/malformed-call coverage have regressions. D29 remains PARTIAL because XML/GUI/Objects mutations and the full language/resolver contract remain incomplete. Debug/Release each passed 4/4 CTest with 131 core passed, 0 failed, 4 skipped. See GALAXY_SPANS.md and core-tests-*-galaxy-spans.txt. The earlier 127-test and portable results above are historical.

GUI continuation: project FunctionDef/ParamDef typing and exact UTF-8 carriers drive analysis, rename, merge and batch merge. Entity-only IDs and unresolved/conflicting types have positive/negative regression coverage. Final Debug and Release each passed 4/4 CTest, 136 core passed, 0 failed, 4 skipped. D17 remains PARTIAL until native external bindings and remaining grammar are covered. No current GUI portable or Editor acceptance is claimed. See GUI_SPANS.md.

Native GUI/CLI continuation (2026-09-28): supplied build-97563 bindings shipped; native/conflict/stale/library/special-type regressions and actual scoped CLI deletion added. Final Debug/Release 5/5 CTest each, 142 core passed / 0 failed / 4 skipped. Resource lookup supports outside-source Debug CLI. D05/D17 remain PARTIAL for the explicitly listed mutation and grammar gaps. See GUI_NATIVE_TYPES.md.

Shared merge preflight continuation (2026-09-28): nonrewritable Strong/Blocking references now reject preview and batch redirects, with scoped target checks and preserved binary diagnostics. Computed/dynamic/opaque Galaxy cases and positive GUI/Galaxy paths are covered. Debug/Release each 5/5 CTest, core 143 passed / 0 failed / 4 skipped. D05/D29 remain PARTIAL; XML mutation and independent verification still need work. See MERGE_REFERENCE_PREFLIGHT.md.

Catalog-scoped merge continuation (2026-09-28): typed GUI/Galaxy overlaps, XSD/parent/UnitBirth XML domains, raw consumer verification without targets and ambiguous unscoped blocking now have four reproduced regressions. Existing identity fixture now uses XSD CUnit.PowerupEffect rather than unsupported CActor.effect, preserving identity/reference assertions. Debug/Release each 5/5 CTest, 147 core passed / 0 failed / 4 skipped. D05/D29 remain PARTIAL for full XML/Objects/case/semantic verification gaps. See MERGE_CATALOG_SCOPES.md.

XML carrier continuation (2026-09-28): unknown XML matches now block only affected catalog identities and preserve independent Safe in a focused fixture; typed SpawnUnit/Alert/DataRecord rewrite and scalar asset-path exclusion are covered. Debug/Release each 5/5 CTest, 150 core passed / 0 failed / 4 skipped. D05/D29 remain PARTIAL: complete span rewrite, raw XML consumer verifier, case equivalence, project layers and Editor acceptance are still open. Probe JSON and details: XML_CARRIER_BOUNDARIES.md. The prior table's 127-test count is historical.


Case/output continuation (2026-09-28): failing-before XML case variant and raw XML no-target verifier reproduced; XML/Galaxy case variants now block affected rename/merge, and post-merge validation reads unknown catalog XML independently of target declarations. Declared CModel.Model asset path remains non-link. Final Debug/Release each 5/5 CTest, 153 core passed / 0 failed / 4 skipped. D05/D25/D29 remain PARTIAL for full case policy, layered/effective model, complete carrier and transaction proofs. See CASE_AND_OUTPUT_VERIFICATION.md. Current probe JSON is benchmark-final-{small,large}.json. Editor NOT_RUN; portable checkpoint historical.


Transaction/portable continuation (2026-09-28): per-file commit recheck and selective conflict-preserving rollback cover external writes after the first commit. Debug/Release each 5/5 CTest, 154 core passed / 0 failed / 4 skipped. Current staged Release and ZIP passed resource/194-entry checks, outside-source layout and read-only map-preview smoke; offscreen OpenGL was unavailable. D25 remains PARTIAL because the hash-check-to-commit window and layer/settings fingerprints are not closed. D26's injected rollback subset remains PASS; external-conflict cases are detected and preserved, not automatically restored. See CONCURRENT_COMMIT_GUARD.md. Editor NOT_RUN; full assignment active.


Unique local default continuation (2026-09-28): D08 remains PARTIAL, but one exact-class local default now contributes typed references in object context; two defaults stay Unknown. Focused rename mutates the default Link successfully. Final Debug/Release each 5/5 CTest, 156 core passed / 0 failed / 4 skipped. See LOCAL_CLASS_DEFAULTS.md. Dependency/layer and scalar/array effective models remain open; the previous ZIP predates this step; Editor NOT_RUN.


Current portable checkpoint after defaults: dist/SC2DataHelper-3.0-beta3-defaults-20260928.zip (SHA-256 c7800217b3393680e27fc619a6531fffa9a02a22a83d0654341e99832b05bb21), 194 ZIP entries verified, outside-source layout and read-only Mercs preview passed. Editor NOT_RUN. Earlier ZIP statements above are historical.


Active component continuation (2026-09-28): D07 remains NOT_IMPLEMENTED for ordered/transitive dependency loading, but explicit ComponentList omission/unknown nested GameData now forces Partial instead of false Safe. Positive gada/GameData fixture remains Complete/Safe. Debug/Release each 5/5 CTest, 157 core passed / 0 failed / 4 skipped. Current ZIP dist/SC2DataHelper-3.0-beta3-components-20260928.zip SHA-256 87785e0a0b7de8a825f64f3a88e2df53a0dac705469af59aef1d42c2cd2f97ed passed 194-entry/resource and outside-source smoke checks. See ACTIVE_COMPONENT_BOUNDARY.md. Editor NOT_RUN; full task active.


Local scalar/provenance continuation (2026-09-28): D08/D09/D23 remain PARTIAL, but explicit empty/0/false and unique local parent/default scalar values now have field/declaration provenance and drive the safe child-node cleanup proof; repeated/removed remain Unknown. Typed links also have original source field paths. Debug/Release each 5/5 CTest, 159 core passed / 0 failed / 4 skipped. Current ZIP dist/SC2DataHelper-3.0-beta3-provenance-20260928.zip SHA-256 042950793f9eec10ebdafa3945bcf0a589934ebf5316731679ec5bf759c8f20e passed 194-entry/resource and outside-source smoke checks. See EFFECTIVE_SCALAR_CONTRACT.md. Editor NOT_RUN; full task active.


Root attribute continuation (2026-09-28): D09/D23 remain PARTIAL. Matching scalar root attributes now use known local effective values and declaration provenance before cleanup can be Safe. Duplicate parent and mixed XML carriers are non-Safe. Debug/Release each 5/5 CTest, 160 core passed / 0 failed / 4 skipped. Current ZIP dist/SC2DataHelper-3.0-beta3-attribute-20260928.zip SHA-256 35e1975f906ad0148b4e6518c49b64bb49d08c949d49535e81baf892f56771bc passed 194-entry/resource and outside-source smoke checks. See EFFECTIVE_SCALAR_CONTRACT.md. Editor NOT_RUN; full task active.

Ordered dependency declaration continuation (2026-09-28): D07 remains NOT_IMPLEMENTED for transitive loading and layer overrides. DocumentInfo now retains ordered structured direct declarations; dependencyGraphComplete=false while unresolved. Real copied maps and two copied mods show direct/transitive dependencies without false Safe. Release/Debug each 5/5 CTest, 161 core passed / 0 failed / 4 skipped. Current ZIP dist/SC2DataHelper-3.0-beta3-dependency-record-20260928.zip SHA-256 91be64df3553681ebe669c3b30e798affd3453967be92eb8065fba4a931c64e4 passed 194-entry/resource and outside-source smoke checks. See DEPENDENCY_DECLARATIONS.md. Editor NOT_RUN; full task active.

Explicit dependency source continuation (2026-09-28): D07 remains PARTIAL. Explicit roots now locate and hash matching archive sources, follow DocumentInfo transitively, and diagnose ambiguity/cycles/traversal; actual GameData layer loading and override semantics remain absent. Copied City with two copied mods produced two Located and four Missing edges, Partial and zero Safe. Release/Debug each 5/5 CTest, 163 core passed / 0 failed / 4 skipped. Current ZIP dist/SC2DataHelper-3.0-beta3-dependency-sources-20260928.zip SHA-256 2fa2f78cf12915aed98f2e4f31e0451a99af0726d66a91d3d139f3558f6dfc13 passed 194-entry/resource and outside-source smoke checks. See EXPLICIT_DEPENDENCY_SOURCES.md. Editor NOT_RUN; full task active.

Dependency GameData layer continuation (2026-09-28): D07 remains PARTIAL. Unique located archives now require active gada/GameData ComponentList and keep parsed declarations separate from local map nodes. Copied City with two copied mods yielded 171 and 1176 dependency objects while local map nodes remained 12,856; missing direct/transitive dependencies keep Partial and zero Safe. Runtime override/effective semantics remain unproven. Release/Debug each 5/5 CTest, 164 core passed / 0 failed / 4 skipped. Current ZIP dist/SC2DataHelper-3.0-beta3-dependency-layers-20260928.zip SHA-256 bc4c2d29199c4c80d8003c0519748c99d4559f10d6180be8b48a99ef3206945e passed 194-entry/resource and outside-source smoke checks. See DEPENDENCY_GAMEDATA_LAYERS.md. Editor NOT_RUN; full task active.

Layered declaration index continuation (2026-09-28): D07/D08/D09 remain PARTIAL. All local and active dependency catalog/ID and exact-class default declarations retain source provenance; cross-layer collisions invalidate known local effective values and propagate through local parents. Copied City: 14,122 identity groups, 80 multi-declaration, 74 cross-source, 6 raw-case variants; still Partial and zero Safe. Runtime winner/array semantics are not established. Release/Debug each 5/5 CTest, 165 core passed / 0 failed / 4 skipped. Current ZIP dist/SC2DataHelper-3.0-beta3-layered-index-20260928.zip SHA-256 c52ceeb66ba08ca59461c25e6fdc96aa3cb57e5523e16f523a264659d81d9c8c passed 194-entry/resource and outside-source smoke checks. See LAYERED_DECLARATION_INDEX.md. Editor NOT_RUN; full task active.

Objects continuation (2026-09-28): D05/D29 remain PARTIAL. The index and mutation now share scoped `Objects` text spans; post-merge verification reads `Objects` against removed IDs even after the target declaration disappears. Focused cross-catalog, comment, unknown-carrier and UTF-16LE regressions passed. Release and Debug each passed 5/5 CTest (102.35 s and 290.61 s). This does not establish XML-form/full Editor grammar or Editor/gameplay acceptance. The current ZIP predates this code. See OBJECTS_REFERENCE_INDEX.md.

Raw array continuation (2026-09-28): D10/D11/D12 remain PARTIAL. Direct and nested repeated field declarations now retain ordinal, exact address, index/removed/value presence and source provenance without multiplying ancestor items per descendant. This is a raw declaration model, not an effective runtime array or Safe cleanup proof. Release/Debug each passed 5/5 CTest (104.15 s/298.96 s); the copied-Mercs archive fixture was verified as present and its three optional tests passed without skips in both configurations. Read-only City remained Partial, 12,805 Unknown and zero Safe; peak 576,417,792 bytes. See RAW_ARRAY_DECLARATIONS.md. Existing ZIP predates this code; Editor NOT_RUN.

Theoretical dependency diagnostic continuation (2026-09-28): D07 remains PARTIAL, and D08-D12 remain PARTIAL. A closed source graph yields a proposed dependency-first layer order; cross-source collisions are exported with original declaration provenance. This does not choose runtime override winners or enable Safe. Release focused tests passed 5/5 QtTest functions with zero skips and the affected Release tests/probe targets compiled. Full Debug/Release, fresh real-map probes, portable and Editor checks after this change are NOT_RUN. See HYPOTHETICAL_LAYER_ORDER.md and THEORETICAL_STATE_RU.md.
