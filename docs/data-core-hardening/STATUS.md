﻿﻿# StarCraft II Data Helper — Beta 4.0 final status

## Beta 4.0 — разработка остановлена 2026-09-29

Проект оставлен на текущем рабочем этапе: Mod Kit покрывает большую часть его
функциональности. StarCraft II Data Helper Beta 4.0 остаётся вариантом для тех,
у кого нет Mod Kit или кому нужен GUI. Выполненные проверки и границы
поддерживаемой модели перечислены ниже; первоначальное полное задание не
завершено. Сложные зависимости, массивы и runtime-семантика не считаются
доказанными только по диагностике Editor. Для неопределённых случаев анализ
сохраняет Partial/Unknown и не предлагает Safe-изменение.

## Последняя реализация 2026-09-29: диагностическое значение с provenance

`LayeredDeclarationIndex.h` теперь выдаёт отдельный Editor-диагноз для точной
прямой двухмодовой формы `CUnit.LifeMax`: B,A → 110 из A; A,B → 220 из B;
локальное значение → 330 из Top. Probe v2 показывает `selected_source`,
`selected_location`, порядок деклараций и `runtime_or_safe_proof=false`.
Вариант с parent, дополнительным активным объектом и все более сложные формы
не получают догадочного значения.
Реальные Editor-моды A/B не содержат отдельного GameData include-файла;
диагноз проверен и на этой форме, и на синтетическом варианте с manifest.
`dependencyGraphComplete=false` и `0 Safe` сохранились на обеих копиях.
Точечный тест и текущие Release/Debug CTest прошли по 5/5. Копия Mercs в
read-only probe осталась Partial: 863 объекта, 853 Unknown, 0 Safe; SHA-256
копии после анализа не изменился.

---

## Предыдущее наблюдение 2026-09-29: Editor показал winner прямых A/B Unit LifeMax

На двух новых копиях верхнего папочного мода без локального `CUnit` Editor
5.0.16.97563 показал итоговый `(Basic) Life Maximum`: 110 при объявленном
порядке B,A и 220 при A,B. Моды A/B задают один ID и значения 110/220;
скриншоты, протоколы и SHA-256 входов сохранены в
`hardening-stage/editor-fixtures-20260929/override-order-v1/override-order-editor-evidence.json`.
На отдельном Top с явным локальным `LifeMax=330` Editor показал 330.
Это доказательство отображаемого Editor-значения для прямого двухмодового
`CUnit.LifeMax` subset, а не общего runtime-порядка, транзитивных слоёв или
массивов. Destructive gate пока не менялся.

---

## Предыдущее изменение 2026-09-29: абсолютные `file:` пути внутри explicit root

Editor открыл A/B/Top fixture с абсолютными путями к папочным модам. Теперь
resolver принимает такой путь только внутри явно заданного search root.
Регрессия отвергает путь к существующей папке вне root. Read-only probe на
Editor-открытом Top нашёл оба слоя и общий ID: `layers=2`, `located=true`,
`Partial`, `0 Safe`. Текущий Release прошёл 5/5 CTest. Это не подтверждает
runtime winner или правило массивов.

---

## Предыдущее изменение 2026-09-29: папочные зависимости `.SC2Mod`

Resolver теперь читает `DocumentInfo`, `ComponentList` и включённые GameData XML
из папочных модов по явно заданному корню поиска. Для такой зависимости
фиксируется хеш всего набора файлов; добавление или изменение файла после
анализа обнаруживается. Порядок применения одноимённых деклараций остаётся
недоказанным: граф отмечен Partial, кандидаты из него не получают Safe.
Новый регрессионный тест и Release CTest прошли 5/5. На подготовленном A/B-моде
probe увидел два слоя, один общий ID, Partial и 0 Safe. Release ZIP был
проверен по манифесту и после распаковки тем же read-only probe. Проверка
runtime-порядка слоёв и игрового выполнения остаётся открытой.

---

## Предыдущее продолжение 2026-09-29: Editor загрузил A/B по абсолютным путям

Тестовые папочные A/B/Top моды созданы вне пользовательских карт. Editor
отклонил относительные `file:Mods/...`, но загрузил абсолютные A и B:
Data Source перечислил B, A и Top, Units показал общий ID. Поле LifeMax
в скрытом окне не считано, поэтому winner не объявлен. Probe Data Helper
обнаружил конкретный пробел P1: папочные `.SC2Mod`-зависимости остаются
Missing даже при explicit root. См. EDITOR_ACCEPTANCE_20260929.md.

---

## Предыдущее продолжение 2026-09-29: минимальный мод открыт в Data Module

Editor открыл новый папочный SC2Mod и показал созданный Unit по ID в Data
Module; входные файлы не менялись. Это подтверждает пригодность формы fixture
для следующего эксперимента, но поле LifeMax и same-ID winner не считаны.
См. EDITOR_ACCEPTANCE_20260929.md.

---

## Предыдущее продолжение 2026-09-29: неоднозначный parent не даёт known

У локального scalar resolver исправлено ложное `known` для class default при
двух возможных родителях. Явный локальный literal остаётся известным;
цепочка свыше 32 деклараций диагностируется. Новая регрессия прошла, полный
Release — 5/5 CTest, core 178 passed / 0 failed / 4 skipped. Текущий Release
ZIP прошёл вне source tree 7/7 layout profiles и probe Mercs. Текущий Debug
тоже прошёл 5/5 CTest, core 178 passed / 0 failed / 4 skipped.

---

## Предыдущее продолжение 2026-09-29: Editor save/reopen на копии

Editor 5.0.16.97563 сохранил отдельную Mercs-копию, изменив её SHA-256,
после закрытия повторно открыл сохранённый документ. Исходная карта не
изменилась; упакованный probe v2 прочитал сохранённую копию и получил
863 объекта / 853 Unknown / 0 Safe / Partial. GUI compile, gameplay,
override/array и приёмка destructive-результата всё ещё не проверены.
См. EDITOR_ACCEPTANCE_20260929.md.

---

## Предыдущее продолжение 2026-09-29: применённое удаление и честный отчёт

CLI на новой локальной fixture удалил один доказанно неиспользуемый Effect;
при отсутствующем моде оставил XML нетронутым и сообщил
`unusedApplyMode=no-safe-candidates`. Отчёты и входы лежат в
`hardening-stage/semantic-examples-20260929`; пример описан в README.
После исправления отчёта текущий Release прошёл 5/5 CTest, core 177 passed /
0 failed / 4 skipped. После этого текущий Debug тоже прошёл 5/5 CTest,
core 177 passed / 0 failed / 4 skipped. Актуальный ZIP прошёл portable
layout/probe smoke после распаковки.
См. ACCEPTANCE_20260929.md.

---

## Предыдущее продолжение 2026-09-29: единственный путь в архиве

Архивный resolver больше не выбирает первый case-insensitive match для
`DocumentInfo`, ComponentList, GameData manifest или Catalog include. Два
варианта дают диагностированную неполноту. Первый полный Release обнаружил
несовместимость текста старой диагностики ComponentList; она исправлена.
Финальные после патча Release и Debug: по 5/5 CTest, core 176 passed /
0 failed / 4 skipped. Свежие read-only Mercs/City пробные отчёты остаются
Partial и zero Safe. Актуальный ZIP указан в ACCEPTANCE_20260929.md;
Editor ранее открыл диагностическую копию Mercs и показал её имя в отвечающем окне;
более поздний цикл save/reopen описан выше. Gameplay остаётся NOT_RUN.
См. EDITOR_ACCEPTANCE_20260929.md.

---

## Проверенное продолжение 2026-09-29: include parity и probe v2

Один GameData manifest parser используется папкой и архивом. Папочный
анализ включает только перечисленные Catalog XML, хранит соседние XML как
неактивные raw источники и делает недопустимый include path причиной Partial.
Синтетические folder/archive проверки прошли 4/4 функций; полный текущий
Release и Debug прошли по 5/5 CTest, core в каждом — 175 passed / 0 failed /
4 skipped. Четыре отсутствующие fixtures не засчитываются как успех.

Свежий read-only probe v2 даёт parseable JSON с input fingerprints: Mercs
863 объектов / 853 Unknown / 0 Safe; City 12 856 / 12 805 Unknown / 0 Safe.
Обе карты Partial; в City найдены два архива модов, четыре транзитивных ребра
отсутствуют. На локальном reference-snapshot liberty.sc2mod manifest исключил
110 соседних XML; версия игры этого snapshot не установлена. Актуальный
Release stage упакован с документами и проверен вне source tree: 7 layout
profiles PASS, packaged probe прочитал копию Mercs и схему. OpenGL context
в headless smoke не создан. Editor/gameplay: NOT_RUN. Runtime winner,
межслойный effective scalar и effective arrays остаются открытыми.
Подробности: SUPPORTED_MODEL.md, ACCEPTANCE_20260929.md, PROBE_JSON_V2.md,
EDITOR_ACCEPTANCE_20260929.md.

---

## Earlier continuation: theoretical dependency diagnostics — 2026-09-28

The dependency source graph exposes a diagnostic closure flag and proposed
dependency-first order without changing the incomplete/Safe gate. Layered
declarations follow that proposal when available, otherwise discovery order;
archive filenames no longer decide precedence in the diagnostic index. The
probe and human report expose the order, and the probe lists cross-source
identity collisions with declaration provenance. Release focused QtTest passed
5/5 functions (0 failed/skipped) and both affected Release targets compiled.
Dependency archives with a `Base.SC2Data/GameData.xml` Catalog include list now
load only listed XML in manifest order; unlisted XML remains visible as inactive.
An updated synthetic archive fixture passed 3/3 focused QtTest functions with
zero failures/skips, and the Release tests/probe targets compiled.
Full Debug/Release rerun, fresh real-map probe, portable and Editor checks are
deferred. See HYPOTHETICAL_LAYER_ORDER.md and THEORETICAL_STATE_RU.md. The
runtime override winner/effective array model and full assignment remain open.

---

The active model now records direct and nested XSD-declared repeated fields
with separate source ordinals, exact XML addresses, raw `index`/`removed`/value
presence and declaration provenance. Each source owns its records once; a
descendant retains an ordered source chain instead of copying ancestor arrays.
No effective append/replace/reset rule is inferred. A nested/empty/removed
regression and full Release/Debug CTest passed 5/5 (104.15 s/298.96 s), with
three copied-Mercs archive tests explicitly passing with zero skips in both
configurations. See RAW_ARRAY_DECLARATIONS.md.

Read-only copied Mercs: 863 objects, 5,186 raw array declarations, 2,907 ms,
166,014,976 peak bytes, 853 Unknown/zero Safe. Copied City: 12,856 objects,
54,354 declarations, 42,150 ms, 576,417,792 peak bytes, 12,805 Unknown/zero
Safe. Both remain Partial. This memory cost is an open performance issue;
single runs do not isolate it from other recent changes. Current ZIP predates
this code; Editor/gameplay acceptance is NOT_RUN. Full task remains active.

---
# Current continuation: post-merge Objects verification - 2026-09-28

Post-merge validation now reads `Objects` directly against removed IDs even
after those declarations have left the rebuilt catalog index. It distinguishes
typed Unit/Doodad scopes, ignores comments, blocks unknown matching carriers,
and accepts valid UTF-8/UTF-16LE source while rejecting unsupported encodings.
The reference index uses the same encoding boundary. Focused no-target and
UTF-16LE regressions passed; full Release and Debug each passed 5/5 CTest
(102.35 s and 290.61 s). See OBJECTS_REFERENCE_INDEX.md.

This is a lexical safety check, not Editor-semantic acceptance. Existing ZIP
predates this code; full assignment remains active.

---
# Current continuation: shared Objects reference spans - 2026-09-28

`Objects` indexing and mutation now share the same scoped text-form scanner.
Known `ObjectUnit` and `ObjectDoodad` carriers produce catalog-specific,
rewritable placement references. Unknown matching tokens block mutations;
comments do not produce references. Incomplete scans also block matches in
the unparsed suffix and make analysis Partial. A focused cross-catalog/comment
regression passed. Full Release and Debug each passed 5/5 CTest (99.98 s and
286.16 s). See OBJECTS_REFERENCE_INDEX.md and OBJECTS_MUTATION_SPANS.md.

The existing ZIP predates this continuation. Editor validation is NOT_RUN.
The full assignment remains active: dependency precedence/effective arrays,
complete Objects/XML and actor grammars, definitive case policy, independent
semantic verification, and Editor/gameplay acceptance remain open.

---
# Current continuation: layered declaration index - 2026-09-28

The active model now indexes all named catalog/ID declarations and exact-class defaults from local and dependency GameData with original source locations and ID spelling. Cross-layer identity/default collisions and affected local parent chains invalidate known effective values rather than choosing an arbitrary override. Copied City with two copied mods yielded 14,122 identity groups, 80 multi-declaration groups, 74 cross-source identities and 6 raw-case variants. City remains Partial and zero Safe. See LAYERED_DECLARATION_INDEX.md.

Release/Debug each passed 5/5 CTest, 165 core passed / 0 failed / 4 unavailable fixture skips. Current ZIP: dist/SC2DataHelper-3.0-beta3-layered-index-20260928.zip, SHA-256 c52ceeb66ba08ca59461c25e6fdc96aa3cb57e5523e16f523a264659d81d9c8c, 194 verified entries and outside-source layout/map-preview smoke. Editor NOT_RUN. Full task remains active: runtime dependency precedence, cross-layer effective values, arrays/index/removed, mutation grammar, atomicity and Editor acceptance are open.

---
# Current continuation: separate dependency GameData layers - 2026-09-28

Uniquely located dependency archives now require a supported active gada/GameData component before their catalog XML is read. Their DataNodes, XML entries, source hash and diagnostics remain separate from local map nodes and destructive candidates. On a copied City map, the two available copied mods contributed 171 and 1176 dependency objects across 9 and 16 XML files; local objects remained 12,856. The graph is still incomplete, Partial and zero Safe. No runtime override order is inferred. See DEPENDENCY_GAMEDATA_LAYERS.md.

Release/Debug each passed 5/5 CTest, 164 core passed / 0 failed / 4 unavailable fixture skips. Current ZIP: dist/SC2DataHelper-3.0-beta3-dependency-layers-20260928.zip, SHA-256 bc4c2d29199c4c80d8003c0519748c99d4559f10d6180be8b48a99ef3206945e, 194 verified entries and outside-source layout/map-preview smoke. Editor NOT_RUN. Full task remains active: dependency override/effective value semantics, arrays/index/removed, complete mutation grammar, atomicity and Editor acceptance are open.

---
# Current continuation: explicit dependency source discovery - 2026-09-28

An explicit dependency-root option now locates matching file: archives, follows their DocumentInfo transitively, records source hashes, and distinguishes missing, ambiguous, cyclic, unsafe-path and metadata failures. A copied City map with two staged copied mods produced six edges: two Located and four Missing, including two transitive Swarm sources. This is metadata discovery only; dependency catalog data and runtime override precedence are not resolved. City stays Partial, dependency graph incomplete and zero Safe. See EXPLICIT_DEPENDENCY_SOURCES.md.

Release/Debug each passed 5/5 CTest, 163 core passed / 0 failed / 4 unavailable fixture skips. Current ZIP: dist/SC2DataHelper-3.0-beta3-dependency-sources-20260928.zip, SHA-256 2fa2f78cf12915aed98f2e4f31e0451a99af0726d66a91d3d139f3558f6dfc13, 194 verified entries and outside-source layout/map-preview smoke. Editor NOT_RUN. Full task remains active: semantic layer loading, effective arrays/index/removed, mutation grammar, atomicity and Editor acceptance are open.

---
# Current continuation: ordered dependency declarations - 2026-09-28

DocumentInfo dependencies now retain source, ordinal, raw spelling, Battle.net handle and normalized file fallback. Unloaded declarations set dependencyGraphComplete=false instead of falsely reporting a complete graph. Real Mercs has one direct declaration; City has four, and copies of two visible mods disclose further Swarm dependencies. Both map probes remain Partial and zero Safe. See DEPENDENCY_DECLARATIONS.md.

Release/Debug each passed 5/5 CTest, 161 core passed / 0 failed / 4 unavailable fixture skips. Current ZIP: dist/SC2DataHelper-3.0-beta3-dependency-record-20260928.zip, SHA-256 91be64df3553681ebe669c3b30e798affd3453967be92eb8065fba4a931c64e4, 194 verified entries and outside-source layout/map-preview smoke. Editor NOT_RUN. Full task remains active: explicit/transitive loading, layer overrides/effective arrays, mutation grammar, transaction proof and Editor acceptance are open.

---
# Current continuation: root scalar attribute proof - 2026-09-28

Root scalar attributes now carry local effective values and exact declaration provenance. Deep cleanup marks an equal child/parent attribute Safe only after proving both known values and a unique local parent. Ambiguous duplicate parents and simultaneous attribute/child carriers remain non-Safe. Debug/Release each passed 5/5 CTest, 160 core passed / 0 failed / 4 unavailable fixture skips. See EFFECTIVE_SCALAR_CONTRACT.md.

Fresh read-only probes on copies: Mercs 2,775 ms / 158 MiB / 863 objects / 13,212 references / 853 Unknown; City 40,358 ms / 504 MiB / 12,856 objects / 149,502 references / 12,805 Unknown. Both Partial from unresolved dependencies, zero Safe. Single-run time differences are inconclusive. See benchmark-attribute-{small,large}.json.

Current portable checkpoint: dist/SC2DataHelper-3.0-beta3-attribute-20260928.zip, SHA-256 35e1975f906ad0148b4e6518c49b64bb49d08c949d49535e81baf892f56771bc; 194 verified ZIP entries, outside-source layout and read-only Mercs map preview passed. Editor NOT_RUN. Full assignment remains active: dependency layers, arrays/index/removed, full mutation grammar and independent verification, case policy, transaction atomicity and Editor acceptance remain open.

---
# Current continuation: local scalar values and declaration provenance - 2026-09-28

The active resolver now records explicit and locally inherited non-repeated scalar values with source address, declaration, presence and raw/effective spelling. Empty, 0 and false remain distinct from an undeclared field. A unique exact-class local default and unique parent chain overlay in order; repeated scalar carriers and removed remain Unknown. Typed links now carry original XML coordinates too. Deep cleanup's safe child-node removal requires equal known scalar results from the actual resolver. Failing-before and positive/negative evidence: EFFECTIVE_SCALAR_CONTRACT.md. Debug/Release each passed 5/5 CTest, 159 core passed / 0 failed / 4 unavailable fixture skips.

Current read-only probes on copied maps: Mercs 2,718 ms / 158 MiB, City 39,950 ms / 504 MiB. Large-map memory is 31 MiB above the preceding checkpoint to retain provenance; runtime change is within single-run variance. Both maps remain Partial due unresolved dependencies. Current portable checkpoint: dist/SC2DataHelper-3.0-beta3-provenance-20260928.zip, SHA-256 042950793f9eec10ebdafa3945bcf0a589934ebf5316731679ec5bf759c8f20e; 194 verified ZIP entries and outside-source layout/preview smoke. Editor NOT_RUN.

Full assignment remains active. Ordered/transitive dependency and override model, complete effective arrays/index/removed semantics, root attribute proof, complete XML/Objects spans and runtime grammar, definitive case policy and cross-process atomic commit proof remain open.

---
# Current continuation: active GameData component boundary - 2026-09-28

ComponentList is now read from folder/archive sources. GameData XML omitted by an explicit list, malformed/unreadable component lists, multiple lists and nested GameData outside the selected local component make analysis Partial instead of producing false Safe proposals. A declared gada/GameData component keeps the closed positive fixture Safe. Reproduced before/after cases and real map metadata are in ACTIVE_COMPONENT_BOUNDARY.md. Debug/Release each passed 5/5 CTest, 157 core passed / 0 failed / 4 unavailable fixture skips.

Current read-only probes on archive copies: Mercs 2,714 ms / 158 MiB / 863 objects / 13,212 references / 853 Unknown; City 39,810 ms / 473 MiB / 12,856 objects / 149,502 references / 12,805 Unknown. Both Partial due to unresolved direct dependencies. Current portable checkpoint: dist/SC2DataHelper-3.0-beta3-components-20260928.zip, SHA-256 87785e0a0b7de8a825f64f3a88e2df53a0dac705469af59aef1d42c2cd2f97ed, 194 verified ZIP entries; outside-source layout and read-only map-preview smoke passed. Editor NOT_RUN.

Full assignment remains active. Ordered/transitive dependencies, override/effective-value provenance, full XML/Objects typed spans and runtime grammar, complete case policy and cross-process atomic commit proof remain open. Unsupported cases are kept Partial/Unknown rather than accepted as Safe.

---
# Current continuation: unique local class defaults - 2026-09-28

The resolver now carries typed references and tokens from one local default declaration of the exact same class into a named object's context. A failing-before regression exposed the missing Consumer->DefaultWeapon edge; the positive test now also renames the weapon and checks that the default Link changes. Multiple local defaults produce an unresolved-layer diagnostic without arbitrary selection. Debug/Release each passed 5/5 CTest and 156 core passed / 0 failed / 4 unavailable fixture skips. See LOCAL_CLASS_DEFAULTS.md.

Current read-only Release probes on archive copies: Mercs 2,784 ms / 158 MiB, City 40,028 ms / 473 MiB. The same 0 Safe and 853/12,805 Unknown candidates remain because dependency/effective coverage is unresolved. D08 improves for a unique local default's typed links, but class defaults across layers, full scalar/array effective values and provenance remain incomplete. Current portable checkpoint: dist/SC2DataHelper-3.0-beta3-defaults-20260928.zip, SHA-256 c7800217b3393680e27fc619a6531fffa9a02a22a83d0654341e99832b05bb21, 194 verified ZIP entries. Outside-source layout and read-only map-preview smoke passed. Editor NOT_RUN. Full task remains active.

---
# Current continuation: concurrent commit and portable checkpoint - 2026-09-28

Folder transactions now hash-check each target immediately before its write and roll back only files they actually committed. If another writer changes a committed output, rollback preserves those bytes and reports a conflict rather than clobbering them. The deterministic regression covers edits to a pending file and to a committed file. Debug/Release each passed 5/5 CTest with 154 core passed / 0 failed / 4 unavailable fixture skips. See CONCURRENT_COMMIT_GUARD.md. A microscopic race between the check and QSaveFile commit remains without a cross-process lock.

Current portable checkpoint: dist/SC2DataHelper-3.0-beta3-core-20260928.zip, SHA-256 d334368b4d8812666356d7eda6f8398a6c6c4a52d731f56f262159446515482d; all 194 ZIP entries verified. App layout smoke and read-only Mercs map-preview smoke passed outside source; map copy unchanged. This checkpoint includes current code and resources, but the full task is still incomplete. The Editor is NOT_RUN. Next: ordered components/dependencies, class defaults/effective values/provenance, complete typed mutation spans and grammar, layer/settings guards and Editor acceptance.

---
# Current continuation: case variants and output XML verification - 2026-09-28

A failing regression showed that a case-folded typed reference could leave a dangling original spelling after rename. Rename and merge now reject affected case variants for XML and Galaxy, using literal source tokens; this remains a conservative boundary pending Editor case-policy evidence. Post-merge validation independently reads raw catalog XML and detects unknown carriers still containing a removed ID after its declaration disappears. XSD-known asset paths remain unaffected. Final Debug and Release each passed 5/5 CTest and 153 core passed / 0 failed / 4 unavailable fixture skips. See CASE_AND_OUTPUT_VERIFICATION.md.

Fresh sequential current Release probes on archive copies: Mercs 2,743 ms / 158 MiB / 863 objects / 13,212 references / 853 Unknown; City 39,124 ms / 472 MiB / 12,856 objects / 149,502 references / 12,805 Unknown. Zero Safe on these maps reflects unresolved external dependency/effective semantics; focused closed fixtures retain positive Safe. Single-run cache variance prevents a speedup claim. JSON and logs are in this directory.

The full assignment remains active and incomplete. Next: project components, ordered dependency layers, class defaults and effective values with provenance; complete XML/Objects mutation spans and raw verifier; token/actor grammar; settings/concurrent transaction guards; current portable build and Editor acceptance. Existing ZIP predates this code, and Editor is NOT_RUN.

---
# Current continuation: XML carrier boundaries - 2026-09-28

Unknown catalog XML attributes/values/text matching a known identity now create catalog-scoped Blocking evidence with ordinal source locations. This closes a reproduced false-Safe deletion; independent targets can still be Safe in the focused fixture. Typed XSD Link carriers drive rename/merge, while declared scalar asset paths stay unchanged. Positive SpawnUnit/Alert/DataRecord cases and negative nested unknown id/parent cases have regressions. Both Debug and Release passed 5/5 CTest with 150 core passed / 0 failed / 4 unavailable fixture skips. See XML_CARRIER_BOUNDARIES.md.

Fresh read-only copies: Mercs 2,754 ms/158 MiB; City 39,997 ms/467 MiB in single Release runs. Prior current-code probes: 2,728 ms/158 MiB and 49,448 ms/465 MiB. Original baseline: 2,006 ms/47 MiB and 38,384 ms/238 MiB. No repeat/cold-cache speed claim. Current probes show zero Safe candidates because dependency/effective coverage is unresolved. Full assignment remains active: project layering/default/effective values, XML/Objects spans, complete runtime grammar, independent raw XML verifier, case/settings/concurrent guards, final portable and Editor acceptance are incomplete. Existing ZIP is a historical checkpoint.

---
# Current continuation: catalog-scoped merge and raw consumer verification - 2026-09-28

Four reproduced merge defects are fixed: typed GUI/Galaxy redirects were lost for cross-catalog same IDs; Actor parent was rewritten during Unit merge; flat-ID post-audit rejected a valid retained Actor link; post-audit missed dangling typed consumers after target removal. Unscoped strong text consumers with cross-catalog ambiguity now block removal. Single and batch paths have positive/negative coverage. The raw GUI/Galaxy verifier reads consumers independently of known target declarations. Final Debug/Release each 5/5 CTest, 147 core passed / 0 failed / 4 unavailable fixture skips; actual archive checks used the Mercs copy. See MERGE_CATALOG_SCOPES.md.

Full assignment remains active and incomplete. Precise XML/Objects carrier rewriting and independent raw XML verification, consistent case policy, token/runtime identity equivalence, dependencies/default/effective values and concurrency/settings/performance work remain open. Unknown unique-ID XML/actor grammar still has a legacy compatibility fallback. D05 and D29 remain PARTIAL. The portable checkpoint below is historical; Editor NOT_RUN.

---

# Current continuation: shared merge preflight - 2026-09-28

Merge preview and batch selection now reject nonrewritable Strong/Blocking references from the shared reference index, scoped by catalog. Computed, dynamic and opaque Galaxy consumers have a failing-before/passing-after regression covering preview, single apply, batch apply and unchanged input bytes. Positive GUI/Galaxy rewrites and binary diagnostics remain verified. Debug and Release each passed 5/5 CTest, 143 core passed / 0 failed / 4 unavailable fixture skips. Actual archive checks ran on the Mercs copy. See MERGE_REFERENCE_PREFLIGHT.md.

Full assignment remains active and incomplete. Typed XML mutation, cross-catalog redirects, independent semantic verification and the dependency/default/effective-values model remain pending. The portable package below is a historical checkpoint and does not contain this change. Editor NOT_RUN.

---

# Current continuation: Native GUI types and scoped CLI - 2026-09-28

NativeLib build-97563 snapshot is now shipped with source/artifact hashes, deterministic offline generator and runtime invalidation. Real Ntve UnitCreate typing, conflicting definitions, library identity, special catalog-facing Unknowns and resource stale checks have regressions. CLI deletion verification and report use catalog+ID; its Debug resource lookup works outside the source tree. Final Debug/Release: 5/5 CTest each, 142 core passed / 0 failed / 4 skipped. Read-only real archive tests ran on a copy. Details: GUI_NATIVE_TYPES.md.

Fresh app/probe/CLI passed outside-project smoke and a real copied-fixture delete, preserving the referenced Unit and exact GUI bytes. Editor NOT_RUN. Benchmarks and original archive hashes are retained. Large-map analysis is about 29% slower in the current repeat and memory almost doubles relative to baseline; no acceleration claim.

The whole task remains active and incomplete. Next: all typed XML/merge carriers and scoped redirects/Unknown preflights, independent output verification, then project components/dependency/default/effective-values model, builtin tokens, concurrency/settings guards and remaining fidelity/performance audit. D05 remains PARTIAL because merge mutation coverage is incomplete. Historical reports below do not supersede this state.

Portable checkpoint: dist/SC2DataHelper-3.0-beta3-gui-types-20260928.zip; 317 manifest files and ZIP content verified. SHA256 is in gui-types-zip.sha256.txt. This checkpoint does not close the full task.

---

# Current continuation: GUI typed references - 2026-09-27

Project GUI FunctionDef/ParamDef typing now drives reference analysis, exact-span rename, merge preview/apply, and batch merge. String values/comments retain their original bytes. Missing/conflicting types block the affected redirect; independent catalog roots remain scoped. GUI .xml documents and entity-encoded IDs have regression coverage. Five new core regressions bring the suite to 136 passed / 0 failed / 4 unavailable fixture skips in final Release, 4/4 CTest PASS. Final Debug also passed 4/4 CTest, core 136 passed / 0 failed / 4 skipped (246.01 s total). Details and before/after evidence: GUI_SPANS.md.

The whole task remains active and incomplete. Native external definition bindings, full dependency/effective-values resolver, XML/Objects mutation typing, concurrent dependency/settings guards, and independent semantic verifier are still pending. Editor NOT_RUN; no new portable artifact is claimed for this GUI code. The Galaxy package below is historical. Next step is versioned NativeLib bindings from the independently inspected supplied source, followed by the remaining full-plan work.

---

# Продолжение: типизированные диапазоны Galaxy — 27.09.2026

Текущий этап: общая лексика Galaxy подключена к анализу, rename и merge. Поддержанные литералы UnitCreate/CatalogFieldValueGet/Set/GetAsInt заменяются по точному диапазону и каталогу. Комментарии и StringToText сохраняются; неизвестные строки с ID блокируют изменение. Простые global const/aliases/concatenation разрешаются только без циклов, неоднозначности и скрытия локальными объявлениями. Подробности и доказательства: GALAXY_SPANS.md.

Полный Debug: 4/4 CTest PASS, 131 passed / 0 failed / 4 skipped, core 225.511 s, total 242.58 s. Release: 4/4 CTest PASS, 131 passed / 0 failed / 4 skipped, core 67.677 s, total 79.74 s. Оба запуска использовали реальную копию Mercs Episode 2 через SC2DH_TEST_ARCHIVE. Четыре отсутствующих audit/model fixtures не считаются PASS. Существующие проверки сохранены; пять условных Galaxy fixtures используют настоящие вызовы вместо произвольного текста и вывода ID.

Исходная задача остаётся незавершённой. Все ограничения layered/effective values, GUI gamelink, XML/Objects mutation heuristics, concurrent dependency/settings guards и независимого verifier сохраняются. Editor: NOT_RUN. Ниже сохранён отчёт предыдущего этапа и его отдельная сборка; текущий portable и новые замеры описаны в GALAXY_SPANS.md.

---
# Data core hardening — состояние 2026-09-27

Ветка codex/data-core-hardening, baseline 3301dd13bc7f1fb10f205867de05257bfd4a17d6. Проверенный этап интегрирован и упакован. Полное задание НЕ завершено; ориентировочная оценка объёма — 60%. Изменения оставлены в рабочем дереве, push/publication не выполнялись. Соседний проект и оригинальные карты не изменялись.

## Реализовано

- Общая политика pugixml сохраняет PI/comments в затронутых core mutation paths. Token PI участвует в сравнении эквивалентности; no-op removal возвращает точные исходные байты. Изменение XML может менять форматирование, но сохраняет неизвестные данные.
- Offline generator сохраняет вложенный XSD AST, QName/namespace, named/anonymous scope, carriers, inheritance, min/maxOccurs/facets и provenance. Runtime использует JSON без Python. Catalog classes берутся из identity selectors: RequirementNode отделён от Requirement. Unsupported XSD/runtime cases не являются доказанной семантикой массивов.
- Project XSD сохранён: SHA256 49b36d9f9b529afe23d0a7f42672f2eff2eafd3352d5eebc422ca24cda6d77de. Supplied XSD: cf5ac8c452a695f9504048481c16422719f8e2a07a898dca0a9802d100390488. Независимое структурное сравнение без annotations подтвердило равенство; детали schema-provenance.json.
- Индекс проверяет hash активного XSD. Cache содержит immutable generations, обновляется при анализе. Snapshot проверяет JSON+XSD в начале/конце анализа и перед apply; missing/incompatible schema блокирует destructive apply.
- Верхние декларации и default без ID отделены от nested ID. Identity — catalog+ID с сохранением прежней case-fold политики приложения; runtime case sensitivity не подтверждена.
- Локальный parent разрешается только в своём каталоге. Missing/ambiguous parent и cycle диагностируются. Token PI родителя/ребёнка разрешается в контексте потомка с ограниченной рекурсией. Typed edges/carriers/provenance используются в графе удаления и unified reference preflight.
- Ссылки предков объединяются консервативно. Это НЕ общий effective-values resolver. Builtin tokens, layer override, array merge/removed/reset остаются неподдержанными.
- Galaxy UnitCreate поддерживает literal/const string/concatenation. Mutable/dynamic ID даёт Unknown для Unit; независимый Effect остаётся Safe. Это ограниченный extractor, не Galaxy compiler.
- Actor On понимает attribute и child Terms/Send. UnitBirth.ID + Create, в том числе token, связывает Unit и Actor. Отсутствующая локальная цель не доказывает допустимость удаления события.
- Удаление учитывает входящие рёбра сохраняемых деклараций. Одноимённый объект другого каталога не скрывает dangling reference; удаление одного duplicate declaration сохраняет identity оставшегося. Полностью выбранный известный подграф/SCC сохраняет действующий положительный путь.
- Deep cleanup допускает равный известный scalar; arrays/index/removed/On/token cases остаются review-only. Auto-merge по body hash выключен.
- DocumentInfo с незагруженными dependencies, broken XML и oversized/unreadable sources даёт Partial. Source/local graph coverage показан отдельно от semantic Unknown.
- Устранены повторные XML parses token/reference-pass внутри descendant context и повторные schema-scope обходы одного class.

## Проверки и артефакт

Baseline Debug CTest: 3/3 PASS, core 51.05 s. Failing regressions сохранены ДО соответствующих исправлений: XML/PI, token context, typed identity, Galaxy, array review, cache, incoming declaration identity, dependencies, actor token root.

Итоговый Debug: 4/4 CTest PASS, core 127 passed / 0 failed / 4 skipped, 203.96 s, total 213.21 s. Release: 4/4 CTest PASS, core 127 passed / 0 failed / 4 skipped, 79.93 s, total 95.15 s. Пять offline generator tests входят в CTest. Три архивных теста реально выполнены через SC2DH_TEST_ARCHIVE на копии Mercs Episode 2. Пропущены Gargantua reference/apply, ZombieWorld audit и M3 fixture; пропуски не считаются PASS.

Старые тесты не удалялись. Fixtures получили корректные same-catalog parents/default collection templates и XSD scalar LifeMax/EnergyMax вместо вымышленных полей. Missing-local-actor ожидание изменено на сохранение; положительная проверка пустого On сохранена.

Первый Release headless запуск не прошёл из-за отсутствия qoffscreen. Stage исправлен, ресурсы и plugin теперь копируются; повторные проверки прошли. Cache test после Stage подменяет активный appdir JSON с RAII-восстановлением, а не неактивный cwd shadow.

Portable: dist/SC2DataHelper-3.0-beta3-data-core-hardening-20260927 и одноимённый ZIP. Включены Qt/CRT DLL, plugins, XSD+JSON, offline scripts, probe/CLI, docs и manual-editor. Приложение и schema probe проверены вне source tree с PATH только Windows/System32; smoke exit 0, семь layout profiles без reported failures. Offscreen OpenGL context отсутствует: это startup/layout проверка, не GPU acceptance. SHA256 файлов находится в artifact-manifest.json, ZIP hash — portable-zip.sha256.txt.

PERFORMANCE.md содержит последовательные baseline/after замеры и raw JSON. Время на большой карте сопоставимо, на малой выросло; память существенно выросла. Обещания ускорения нет. Хеши оригиналов и копий совпадают после проверок.

Editor: NOT_RUN. Три XML-входа независимо проходят XSD, но не являются Editor-saved golden outputs. manual-editor/README_RU.md задаёт реальные проверки загрузки/сохранения/запуска на новых картах.

## Незавершённые обязательные критерии

1. Transitive dependencies/layers, default/effective values, empty/0/false, structs и indexed arrays с независимыми Editor-свидетельствами.
2. GUI FunctionDef/ParamDef gamelink typing, полная Actor grammar, Galaxy scopes/assignments и builtins.
3. Все mutation paths ещё НЕ переведены на установленные typed reference spans. Rename/merge сохраняют legacy heuristics и широкие text rewrites; полный rename-контракт не достигнут.
4. Dependency/settings snapshot, защита от concurrent schema/dependency change на каждой стадии commit и независимый semantic verifier не завершены. Существующие source transactions/rollback и stale gates сохранены.
5. Неизвестные extension/GUI/Actor грамматики не доказаны полностью покрытыми. Сборка не является автоматическим доказательством безопасности произвольного SC2 проекта.

Следующий этап: Editor-saved dependency/indexed-array/removed/reset fixtures, declaration provenance/effective field layer и перевод rename на typed spans. Не выводить runtime array rules из одного XSD или собственного golden output.
