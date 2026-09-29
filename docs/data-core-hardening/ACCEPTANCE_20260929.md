# Матрица D01–D26 — продолжение 2026-09-29

Статус относится к **текущему исходному дереву** и указанной области, а не к
любому SC2-проекту. `PASS` означает проверенный ограниченный контракт;
`PARTIAL` — работающий subset с указанным недостающим доказательством.
Предыдущая `ACCEPTANCE.md` использует номера D01–D30 из более раннего задания.

| ID | Статус | Свидетельство и точная граница |
|---|---|---|
| D01 | PASS | `xmlTokenNoOpAndMutationPreserveData`: PI/comments и соседние данные сохранены в поддержанном XML rewrite |
| D02 | PASS | `xmlTokenDeclarationsAffectEquivalence`: token PI влияет на сравнение |
| D03 | PASS | `catalogIdentityKeepsIndependentIds`: каталог входит в ключ ID |
| D04 | PASS | `explicitDependencyRootsTraceTransitiveCycleAndHashSources`, `explicitDependencyRootsRejectAmbiguityAndTraversal`, `ambiguousArchiveEntryDoesNotPickCaseVariant`: missing/ambiguous/cycle и case-ambiguous archive metadata не дают complete |
| D05 | PASS | `explicitHandleMappingsLocateTransitiveDependencies`: найденные архивы оставляют `dependencyGraphComplete=false` |
| D06 | PARTIAL | Папочные и архивные зависимости читают ComponentList/GameData include, сохраняют unlisted XML как inactive; неоднозначные case-insensitive archive paths не выбирают произвольно. `folderDependencyLayersAreReadAndPinnedWithoutAssumingPrecedence`: папочные `.SC2Mod` и абсолютные `file:` пути внутри explicit root загружаются, путь вне root отвергается; полный runtime activation контракт не доказан |
| D07 | PARTIAL | Editor 5.0.16.97563 на прямом двухмодовом `CUnit.LifeMax` показал 110 для B,A, 220 для A,B и 330 с локальным Top. `collectEditorLayeredScalarDiagnostics` выдаёт значения и декларации на этих же модах без GameData include-файла и на fixture с manifest; parent или лишний активный объект дают пустой результат. Общий runtime-порядок, транзитивный override и массивы не подтверждены; destructive gate не изменён |
| D08 | PARTIAL | Локальные default/parent/scalar/token и provenance работают; узкий Editor-диагноз прямого `CUnit.LifeMax` сообщает значение и source. Он не входит в destructive `resolvedValues`. `ambiguousParentDoesNotClaimInheritedScalarKnown` сохраняет Unknown при двух родителях. Общая межслойная композиция и builtins остаются Unknown |
| D09 | PASS | `localScalarValuesPreserveExplicitPresenceAndProvenance`: empty/0/false/absent различены в локальном поддержанном subset |
| D10 | PARTIAL | Ambiguous defaults и parent cycle диагностируются; неоднозначный parent и превышение 32 деклараций не выдают унаследованный scalar за известный. Runtime case policy отсутствует, case variants не выбираются |
| D11 | PASS | `arrayDeclarationsKeepOrdinalsMarkersAndProvenance`: raw повторы, индексы и nested addresses сохранены |
| D12 | PARTIAL | Raw indexed/unindexed declarations доступны; effective composition по доказанным правилам ещё не реализована |
| D13 | PARTIAL | `removed` хранится raw; per-field/reset runtime semantics не подтверждены |
| D14 | PASS | `arrayEqualityIsNotRemovalProof`: одинаковый XML массива не разрешает cleanup |
| D15 | PARTIAL | Attribute/child Terms/Send и UnitBirth/Create subset есть; полная Actor grammar отсутствует |
| D16 | PARTIAL | FunctionDef/ParamDef, native bindings и library scopes покрыты fixtures; expression/special contexts не полны |
| D17 | PARTIAL | Literal/const/concat и shadow/dynamic guards проверены; произвольный Galaxy-flow не доказан |
| D18 | PARTIAL | Текстовые Objects и standalone mod external consumer protection есть; все runtime/editor/binary roots не подтверждены |
| D19 | PARTIAL | Типизированные GUI/Galaxy/XML/Objects paths и guards есть; полный rename carrier/case контракт не закрыт |
| D20 | PARTIAL | Source/schema/settings revisions и dependency archive SHA-256 есть; continuous config/consumer/commit proof неполон |
| D21 | PARTIAL | Raw post-merge XML/Objects verifier находит часть оставшихся ссылок после удаления цели; полный независимый semantic verifier отсутствует |
| D22 | PARTIAL | Injected rollback и selective external-conflict guard проверены; кросс-процессная атомарность не доказана |
| D23 | PASS | `componentListExcludesUnlistedGameDataFromSafePlans`: полностью известный локальный Effect остаётся Safe; неисследованный компонент блокирует |
| D24 | PASS | Текущая ревизия прошла Release и Debug по 5/5 CTest; точечная регрессия и read-only Mercs probe прошли. Полное совпадение всех real-map выходов отдельно не доказано |
| D25 | PASS | Release ZIP `SC2DataHelper-4.0-beta-portable-20260929.zip` содержит бинарники Beta 4.0, README, документы и Editor A/B evidence с SHA-256 манифестом. После распаковки probe проверяет 110/220, provenance и 0 Safe. Gameplay acceptance остаётся отдельным |
| D26 | PARTIAL | Editor 5.0.16.97563 открыл, сохранил и повторно открыл отдельную копию Mercs; также показал Life Maximum 110/220/330 на отдельных A/B/Top fixtures с фиксированными входами. Упакованный probe прочитал сохранённую Mercs-копию (863 объекта / 853 Unknown / 0 Safe / Partial). GUI compile, игровой запуск, массивы и приёмка destructive-результата остаются NOT_RUN; см. EDITOR_ACCEPTANCE_20260929.md |

Текущие Release и Debug прошли по 5/5 CTest; логи:
`hardening-stage/full-{release,debug}-editor-scalar-nomanifest-20260929.log`.
Четыре отсутствующие внешние fixtures по-прежнему не засчитываются как успех.
Четыре отсутствующие fixtures не объявляются успешными.

Fresh read-only probes on current Release: Mercs 863 objects / 853 Unknown /
zero Safe; City 12,856 objects / 12,805 Unknown / zero Safe, two located mods
and four missing transitive edges. Both are Partial. v2 JSON parsed in
PowerShell; details and single-run memory/time in `PROBE_JSON_V2.md`.

Portable checkpoint: `hardening-stage/SC2DataHelper-3.0-beta3-editor-open-20260929.zip`,
with the final SHA-256 in the adjacent `.zip.sha256.txt` sidecar. The ZIP is a
checked intermediate build, not full Editor/gameplay acceptance.

Archive case-path continuation: `DocumentInfo`, ComponentList, GameData
manifest and included Catalog XML now require one case-insensitive archive
match. A duplicate cannot become a runtime winner by list order. Initial full
Release caught a changed ComponentList diagnostic; after restoring that
diagnostic, focused regressions passed. The latest full Release and Debug each
passed 5/5 CTest with core 176 passed / 0 failed / 4 skipped. Fresh read-only
Mercs/City v2 probes remain Partial and zero Safe. The previous portable ZIP
predates this continuation.
