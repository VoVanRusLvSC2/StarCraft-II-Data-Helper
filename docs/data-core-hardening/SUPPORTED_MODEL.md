# Поддерживаемая модель данных — 2026-09-29

Модель различает физическое обнаружение, активность источника, замыкание графа
по файлам, порядок слоёв, покрытие ссылок, разрешение поля и доказательство
неиспользования. Эти свойства не сводятся к одному `complete`. Последнее
подтверждение каждой операции задаётся её конкретной областью и preflight.

| Правило / область | Основание | Fixture и код | Влияние на операции |
|---|---|---|---|
| XML PI/comments/unknown extension сохраняются | XML parse policy и no-op/mutation регрессии | `xmlTokenNoOpAndMutationPreserveData`, `XmlParsePolicy.h` | Поддержанный XML rewrite не теряет PI; неизвестный carrier не становится ссылкой по догадке |
| Локальная идентичность: catalog + case-folded ID, исходное написание отдельно | Действующая политика приложения; runtime case policy не доказана | `catalogIdentityKeepsIndependentIds`, `CatalogProtection.h`, `LayeredDeclarationIndex.h` | Разные каталоги независимы; case variants остаются неоднозначными для destructive path |
| Локальный scalar и единственные default/parent | XSD carrier + положительные и отрицательные local fixtures | `localScalarValuesPreserveExplicitPresenceAndProvenance`, `ambiguousClassDefaultsStayUnknownInAffectedCatalog`, `CatalogDataModel.h` | Равный известный scalar может участвовать в ограниченном cleanup; ambiguity блокирует |
| Token PI в контексте потомка | Локальный XML и контекстные fixtures | `tokenContextProtectsResolvedTargets`, `CatalogDataModel.h` | Известная ссылка защищает цель; unknown/cycle не допускает ложный Safe |
| Прямые/транзитивные dependency sources | `DocumentInfo`, явные roots/mappings, SHA-256; абсолютный `file:` принимается только внутри explicit root | `explicitHandleMappingsLocateTransitiveDependencies`, `folderDependencyLayersAreReadAndPinnedWithoutAssumingPrecedence`, `DependencySourceResolver.h` | `Located` не доказывает runtime winner и не снимает incomplete gate; путь вне root отклоняется |
| Активные GameData includes в поддержанной форме | `ComponentList` + `Base.SC2Data/GameData.xml` с `<Includes><Catalog path=.../>`; пример из локального reference-sc2gamedata snapshot без установленной версии игры | `folderGameDataManifestSelectsActiveCatalogs`, archive mapping fixture, `GameDataIncludeManifest.h` | В папке и архиве unlisted XML исключён из активных declarations; raw файл/ревизия сохранены; неизвестные include дают Partial |
| Единственный архивный metadata/include path | Нормализованное сравнение путей с сохранением исходного написания; два case-insensitive совпадения неоднозначны | `ambiguousArchiveEntryDoesNotPickCaseVariant`, `DependencySourceResolver.h` | `DocumentInfo`, ComponentList, manifest и Catalog include не выбирают первый файл; неоднозначность даёт issue/Partial |
| Порядок слоёв при закрытом source graph | Порядок деклараций `DocumentInfo` и dependency-first обход; runtime acceptance отсутствует | `explicitDependencyRootsTraceTransitiveCycleAndHashSources`, `LayeredDeclarationIndex.h` | Только диагностика; ни winner, ни Safe не выводятся из `hypotheticalLayerOrder` |
| Прямые A/B для `CUnit.LifeMax` в Editor | На двух папочных Top-копиях без локального Unit Editor 5.0.16.97563 показал 110 для B,A и 220 для A,B; с локальным 330 показал 330 | `override-order-editor-evidence.json`, снимки и SHA-256 в `hardening-stage/editor-fixtures-20260929/override-order-v1` | Наблюдение ограничено этим Editor Data Module subset; общие runtime winner, транзитивные слои и Safe/apply из него не выводятся |
| Диагностический scalar по наблюдённому subset | Только два прямых папочных `.SC2Mod`, по одному активному `CUnit` с одинаковым написанием ID и простым целочисленным `LifeMax` в каждом, необязательный локальный; без parent/default/token/других полей или активных объектов | `collectEditorLayeredScalarDiagnostics`, `folderDependencyLayersAreReadAndPinnedWithoutAssumingPrecedence`, `editor_observed_scalar_diagnostics` в probe v2 | Показывает 110/220/330 и точную декларацию-источник; остаётся вне resolvedValues, Safe/apply и общего runtime-контракта; parent или лишний активный объект дают пустой результат |
| Raw массивы, включая индекс/ordinal/removed | XSD multiplicity и XML provenance | `arrayDeclarationsKeepOrdinalsMarkersAndProvenance`, `CatalogModelTypes.h` | Query/diff доступны; effective merge и cleanup массива остаются Unknown |
| Поддержанные XML/GUI/Galaxy/Objects ссылки | XSD, NativeLib bindings и конкретные fixtures | `CatalogLinkSchema`, `GuiReferenceSpans.h`, `GalaxyReferenceSpans.h`, `ObjectsReferenceSpans.h`, `UnifiedReferenceIndex.cpp` | Только доказанные spans переписываются; unknown matching carrier блокирует затронутую операцию |
| Source/schema/settings stale и rollback subset | SHA-256 ревизии, транзакционные тесты | `changedSourceRejectsStaleDestructiveApply`, `folderTransactionRollsBackOnValidationFailure`, `BackupManager.cpp` | Известные изменения отклоняются/откатываются; полный cross-process atomicity proof ещё отсутствует |

## Неподтверждённые правила

- Runtime winner same-ID override, межслойные defaults/parents и builtins:
  исходные объявления хранятся, итоговое значение `Unknown`. Следующий
  эксперимент — Editor-saved map + A/B mods с перестановкой объявлений и
  наблюдаемым значением конкретного scalar.
- Numeric/enum indexed array, неиндексированные повторы, nested structs,
  partial override и removed/reset: только raw declarations. Нужен независимый
  Editor/gameplay output для каждой конкретной формы.
- Полная Actor/GUI/Galaxy/Objects grammar и бинарные/external consumers:
  поддерживается перечисленный subset; неизвестная область блокирует
  затронутый destructive result. Нужны независимые positive/negative fixtures.
- Непрерывная защита source/dependency/config при commit и независимый
  semantic post-write verifier покрыты частично. Требуются race/error fixtures
  и отдельное чтение опубликованного результата.

`Safe` означает доказательство **в поддержанной области**, а не отсутствие
текстового совпадения. Оригинальные игровые архивы не используются для apply;
проверки идут на копиях. Версии/хеши стартового состояния —
`START_STATE_20260929.md`; текущая поэлементная матрица и известные пробелы —
`ACCEPTANCE.md`.
