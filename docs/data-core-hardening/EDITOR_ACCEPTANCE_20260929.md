# Editor-проверка, частично выполнена — 2026-09-29

Доступная копия карты: `E:\SK2\SC2_Edition_MCP\hardening-stage\editor-probe\Mercs Editor Probe.SC2Map`.
Её SHA-256 `22d2dc6d6892bff006e0cad9589174168c54342b773f65a0d6ea4b0fabc97729`
совпадает с исходной benchmark-копией. Исходную карту не сохранять поверх.
Portable layout smoke проверяет Qt-приложение отдельно от описанного ниже
наблюдения SC2 Editor.

### Реальное наблюдение Editor на отдельной копии

29 сентября 2026 `SC2Editor_x64.exe` версии **5.0.16.97563** был запущен
скрыто только на дополнительной диагностической копии
`hardening-stage/editor-observation-20260929/Mercs Editor Observation.SC2Map`.
Чужой Editor-процесс до запуска отсутствовал. Собственный процесс PID 23128
показал отвечающее окно с заголовком
`Terrain - [E:/.../Mercs Editor Observation.SC2Map] - StarCraft II Editor`
в нескольких последовательных наблюдениях и завершился с кодом 0 после
закрытия. Полный отчёт: `hardening-stage/editor-observation-20260929/open-copy.json`.
Хеши исходной benchmark-копии и диагностической копии до/после совпали:
`22d2dc6d6892bff006e0cad9589174168c54342b773f65a0d6ea4b0fabc97729`.
Это **PARTIAL** свидетельство загрузки конкретного документа.

### Сохранение и повторное открытие отдельной копии

Скрипт `hardening-stage/editor-observation-20260929/save-reopen-owned-copy.py`
создал новую копию `Mercs Save Reopen 1.SC2Map` и запустил Editor 5.0.16.97563.
После появления отвечающего окна с именем копии он отправил доступную команду
File → Save только этому окну. SHA-256 копии изменился с
`22d2dc6d6892bff006e0cad9589174168c54342b773f65a0d6ea4b0fabc97729`
на `aefd3d6fb7293f6073e25da62659818e97ab6d56d5359c3db4349e6077c92d1b`;
размер — с 3 840 869 до 3 840 927 байт. Собственный процесс был закрыт,
сохранённая копия вновь открыта в новом процессе и показала имя документа.
Её SHA-256 после повторного открытия не изменился. Исходная benchmark-копия
сохранила исходный SHA-256. Полный отчёт: `save-reopen-1.json`.

Поэлементное сравнение MPQ показало 73 записи до и после. Изменились только
`ComponentList.SC2Components` (Editor добавил `<Optimized/>`) и бинарный
`DocumentHeader`; GameData XML не изменился. Отчёт:
`hardening-stage/editor-observation-20260929/archive-entry-diff-1.json`.

Упакованный read-only probe v2 прочитал сохранённую Editor копию:
863 объекта, 853 Unknown, 0 Safe, Partial; отчёт `saved-map-probe-v2.json`.
Это подтверждает цикл load/save/reopen для **этой карты**, но не подтверждает
компиляцию GUI, игровое выполнение, порядок override, effective arrays или
приёмку результата destructive-оптимизации. Эти этапы остаются `NOT_RUN`.

### Минимальный папочный мод: Data Module

Новый `hardening-stage/editor-fixtures-20260929/SC2DH-Minimal-B.SC2Mod`
содержит ComponentList, DocumentInfo и один `CUnit id="SC2DHStandaloneProbe"`
с явным `LifeMax=110`. Read-only probe считал 1 объект и Complete без
incomplete issues. Editor 5.0.16.97563 открыл эту папку как документ,
затем его Data Module показал тип Unit и единственную запись
`SC2DHStandaloneProbe (Unnamed)`; исходные три файла не изменились.
Наблюдения: `minimal-b-editor-open.json`, `minimal-b-data-module-6.json` и
`minimal-b-data-module-4.png` в той же папке. Скрытый Win32 ListView не дал
надёжно выделить строку для чтения поля; `LifeMax` как effective значение
Editor этим опытом **не подтверждён**. Для правила same-ID override всё ещё
нужны A/B/Map с зависимостями и наблюдение значения, а не только имени Unit.

### A/B-зависимости в новом папочном моде

В `hardening-stage/editor-fixtures-20260929/override-order-v1` создано два
новых папочных мода A/B с одинаковым `CUnit id="SC2DHOverrideProbe"` и
разными явными `LifeMax` 110/220, а также верхние BA/AB-варианты с 330.
Editor отклонил вариант `file:Mods/SC2DH-{B,A}.SC2Mod` с сообщениями
`Dependency file could not be found` для обоих файлов даже при рабочем
каталоге рядом с `Mods`; текст — `top-ba-editor-error.json`. Отдельная копия
верхнего мода с абсолютными `file:E:/...` путями открылась. Data Module
показал источники `Core.SC2Mod`, `SC2DH-B.SC2Mod`, `SC2DH-A.SC2Mod`,
`SC2DH-Top-BA-Absolute.SC2Mod` и единый Unit
`SC2DHOverrideProbe (Unnamed)`; отчёт `top-absolute-editor-result.json` и
снимок `top-absolute-editor-data.png`. Файлы тестовых модов не менялись.

Это подтверждает загрузку названных источников в **этом** Editor-документе,
но список Data Source не доказывает runtime precedence. Попытка выбрать
Unit в скрытом Win32 ListView тогда не дала полей (`Field Count: 0`).
Впоследствии выбор видимой строки сработал; новые наблюдения `LifeMax`
описаны ниже. Тот старый probe сообщил Missing и 0 слоёв; текущий resolver
уже читает папочные `.SC2Mod` и абсолютные пути внутри explicit root.

Для определения правила same-ID override приготовлены три минимальных Catalog
в `manual-editor/Override-{B,A,Map}.UnitData.xml`: один ID, значения LifeMax
110/220/330. Их нужно импортировать в отдельные новые моды B и A и новую
тестовую карту Map средствами Editor, чтобы сам Editor создал корректные
metadata и сохранил файлы. Порядок объявления зависимостей сначала B,A,
потом A,B; третьим запуском оставить только B,A и убрать локальное Map
значение. Во всех трёх случаях сохранить карту, закрыть, вновь открыть и
записать показываемое итоговое `LifeMax` вместе с `DocumentInfo`, GameData
manifest, версиями Editor/игры и хешами сохранённых файлов. Если Editor не
позволит импортировать именно эти декларации, сохранить его сообщение — это
отдельное наблюдение, а не повод угадывать итоговое значение.

После этого проверить `manual-editor/ScalarArray.xml` в отдельной карте для
числового/символьного индекса, неиндексированных повторов и removed/reset.
Нужны фактические значения после save/reopen и, если Editor не показывает
итоговое состояние, минимальное игровое наблюдение. XSD-проверка исходного XML
не заменяет эти результаты. Массивные Editor-сценарии пока `NOT_RUN`.

При запуске нельзя закрывать или переписывать чужую несохранённую сессию
Editor. Проверять только созданные копии/новые тестовые карты.

### Наблюдаемый итоговый scalar в Data Module: прямые A/B

В двух новых папочных Top-копиях без локальной декларации `CUnit`
`SC2DHOverrideProbe` зависимости A и B различаются только порядком в
`DocumentInfo`; A задаёт `LifeMax=110`, B — `LifeMax=220`. Обе копии
ссылаются на те же неизменённые папочные A/B через абсолютные `file:` пути.
Editor **5.0.16.97563** открыл каждую отдельно. После выбора Unit в Data
Module поле `(Basic) Life Maximum` показало:

| Порядок в `DocumentInfo` | Значение Editor | Снимок |
|---|---:|---|
| B, A | 110 | `override-order-v1/top-BA-no-local-fields.png` |
| A, B | 220 | `override-order-v1/top-AB-no-local-fields.png` |

Отдельный Top с локальным `LifeMax=330` показал 330 на
`override-order-v1/top-absolute-fields-item.png`. Протоколы выбора поля,
отсутствия изменений источников и SHA-256 входных файлов/снимков собраны в
`override-order-v1/override-order-editor-evidence.json`. Значения прочитаны
с видимой панели Editor, не выведены из порядка Data Source.

Область вывода — отображаемое Editor-значение прямого двухмодового
`CUnit.LifeMax` без parent/default/token/array. Игровое выполнение,
транзитивные зависимости, другие поля и правила массивов этим опытом не
подтверждены. Разрешение Safe/apply по этому наблюдению не включено.

Текущий `SC2CatalogAnalysisProbe` на тех же BA/AB входах выдаёт
`editor_observed_scalar_diagnostics`: 110 из `SC2DH-A.SC2Mod` и 220 из
`SC2DH-B.SC2Mod` с точным XML location. Оба отчёта сохраняют
`dependency_graph_complete=false` и `safe_candidates=0`:
`top-{BA,AB}-editor-scalar-nomanifest.json` рядом со снимками. Это
согласование ограниченного диагностического правила с независимым Editor,
без утверждения о полном runtime/геймплейном контракте.
