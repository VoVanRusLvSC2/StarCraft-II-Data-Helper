# MASTER PROMPT — StarCraft-II-Data-Helper 3.0 Beta 2
## Real-map validation, compatible maximum compression and trustworthy map preview

Ты продолжаешь работу над существующим C++ / Qt 6 проектом:

```text
E:\SK2\Data helper KSP
```

Это продолжение разработки **StarCraft-II-Data-Helper**, а не новый проект и не переписывание с нуля.

Исходная точка этой фазы — как минимум commit:

```text
76124d7 feat: select exact map regions for decor streaming
```

Название целевого проверочного релиза:

```text
StarCraft-II-Data-Helper 3.0 Beta 2
```

Главная цель фазы: перестать доказывать корректность только synthetic-тестами. Нужно реально брать разные пользовательские `.SC2Map` / `.SC2Mod`, создавать отдельно названные оптимизированные копии, проверять сохранность, размер, повторное открытие, Galaxy/Objects и принятие результата настоящим StarCraft II Editor.

Пользователь проведёт окончательное игровое тестирование сам. До него нельзя называть сборку Stable/Final и нельзя скрывать непроверенные ограничения.

---

# 0A. ЛИМИТ 10 ЧАСОВ, МОДЕЛЬ И ПРИОРИТЕТЫ

На весь автономный прогон этого prompt действует жёсткий wall-clock budget **10 часов** с момента начала работы. Используй наиболее сильную и подходящую доступную coding/agent model для сложного C++/Qt, MPQ, SC2 formats и реальных Editor-проверок. Основной выбор: **GPT-5.6 Sol с `xhigh` reasoning**; для самых рискованных safety/format решений допустим `max`, если это не ломает 10-часовой budget. При недоступности или перегрузке Sol допустим **GPT-5.6 Terra с `xhigh` reasoning**. Не трать время задачи на длительное обсуждение выбора модели.

10 часов — это лимит исполнения, а не разрешение объявить непроверенное успешным. Невыполненный тест получает честный `NOT RUN`, `TIMEOUT` или `BLOCKED`, но никогда ложный `PASS`.

Приоритеты:

1. **P0:** безопасность оригиналов, корректность archive write, preflight, fresh verification.
2. **P1:** обязательная миссия, настоящий SC2-map preview, точный выбор Region, корректный responsive UI без выпадающих кнопок.
3. **P2:** Maximum Compatible Compression и доказательство Editor compatibility.
4. **P3:** расширенный corpus, дополнительные visual layers и optional M3 preview.

Ориентировочный timebox, который можно корректировать по фактам, но нельзя превышать суммарно:

| Время | Результат |
|---|---|
| 00:00–00:30 | baseline, worktree audit, manifest, inventory установленного SC2 ModKit |
| 00:30–02:30 | ModKit-backed map data pipeline и настоящий Map Canvas |
| 02:30–03:30 | responsive layout, кнопки, удаление `Actors created per game tick` |
| 03:30–05:00 | preflight/safety и Maximum Compatible Compression |
| 05:00–07:30 | автоматические real-map runs и исправления по фактам |
| 07:30–09:00 | Editor oracle: обязательная миссия и representative matrix |
| 09:00–09:40 | portable package, clean-machine smoke, evidence collection |
| 09:40–10:00 | финальный audit и честный отчёт |

После 8-го часа не начинай optional Level C/M3 работу. Последние 40 минут зарезервированы для сборки, проверки артефактов и отчёта. Если P3 не помещается, урезай P3, а не P0/P1. Не останавливайся ради некритичных уточнений, которые можно безопасно разрешить чтением проекта, установленного ModKit и реальных карт.

---

# 0. ПРАВИЛА, КОТОРЫЕ НЕЛЬЗЯ НАРУШАТЬ

1. Все исходные `.SC2Map`, `.SC2Mod`, `.SC2Campaign`, `.SC2Components` считать пользовательскими активами только для чтения.
2. Никогда не перезаписывать source-карту.
3. Каждая реально оптимизированная карта создаётся только как отдельная явно названная копия.
4. Временные файлы, распакованные архивы, отчёты и проверочные карты хранить только под:

   ```text
   target/diag/beta2-real-maps/
   ```

5. Не добавлять карты, MPQ payload, извлечённые Blizzard assets и generated diagnostics в Git.
6. Перед каждой записью вычислить и сохранить SHA-256 оригинала. После операции доказать, что hash оригинала не изменился.
7. Любой incomplete analysis, parse error, неизвестный archive flag, неподдерживаемый объект, неизвестная ссылка или failed verification переводит destructive change в `BLOCKED`, а не в `Safe`.
8. XML verifier не доказывает, что карту принимает Galaxy Editor. Эти два результата всегда показывать отдельно.
9. Не публиковать GitHub release и не делать push в `public` remote без отдельного явного указания пользователя после ручного теста.
10. Не удалять старые возможности приложения ради упрощения этой фазы.
11. UI widgets изменять только из UI thread. Анализ, декодирование, построение preview, archive rewrite и verification выполнять в worker threads с cancellation.
12. Не делать per-map hacks по имени файла, пути, конкретному ID или заранее известному хешу.

---

# 1. ОБЯЗАТЕЛЬНЫЕ РЕАЛЬНЫЕ ИСТОЧНИКИ

## 1.1 Корпус разных карт и модов

Рекурсивно обнаружить реальные тестовые документы в:

```text
C:\Users\Vladimir\Downloads\TriggerRivezerTests
```

На момент подготовки промпта там было 22 `.SC2Map` / `.SC2Mod`, включая:

- небольшие архивы около 0.5–6 МБ;
- средние архивы;
- большие архивы 100–175 МБ;
- имена с пробелами;
- кириллические имена;
- карты с локальными mod dependencies;
- отдельные `.SC2Mod`.

Нельзя кодировать этот список вручную. Test runner обязан заново сканировать папку, формировать manifest и фиксировать точный набор, размеры и SHA-256 на момент запуска.

## 1.2 Главная пользовательская миссия

Обязательная oracle-карта:

```text
C:\Program Files (x86)\StarCraft II\Maps\Кампания_Империя_KSP_Миссия_1_OPRIMIzATION.SC2Map
```

Важно: не использовать ошибочный вариант, где `Кампания`, `_Империя`, `_KSP` и `_Миссия` интерпретируются как вложенные каталоги. Это единое имя файла в корне `Maps`.

Перед работой всё равно выполнить `Test-Path`/эквивалент и записать фактически найденный canonical path. Если файл отсутствует — не подменять его похожей картой молча; отметить `MISSING_TEST_ASSET` и продолжить остальные тесты.

---

# 2. СНАЧАЛА СОЗДАЙ REAL-MAP TEST HARNESS

Нужен воспроизводимый диагностический runner, который:

1. строит manifest всех входных карт;
2. для каждой карты создаёт собственную output-папку по безопасному slug + short hash;
3. никогда не пишет рядом с original;
4. запускает baseline analysis;
5. сохраняет машинно-читаемый JSON report;
6. выполняет выбранную реальную оптимизацию на копии;
7. повторно анализирует output с нуля;
8. сравнивает structural/semantic invariants;
9. измеряет исходный и итоговый размер;
10. запускает Editor acceptance queue для обязательной выборки;
11. сохраняет итоговый aggregate report.

Добавь воспроизводимую команду проекта, например через существующую build system/diagnostic CLI, но сначала изучи реальные `CMakeLists.txt`, scripts и существующие tools. Не придумывай параллельную build system.

Пример логической команды, точный синтаксис определить по проекту:

```text
Beta2RealMapValidation
  --corpus "C:\Users\Vladimir\Downloads\TriggerRivezerTests"
  --required-map "C:\Program Files (x86)\StarCraft II\Maps\Кампания_Империя_KSP_Миссия_1_OPRIMIzATION.SC2Map"
  --output "target/diag/beta2-real-maps"
```

Runner должен уметь продолжить очередь после одной повреждённой или неподдержанной карты. Один failure не должен уничтожать результаты остальных.

Если карта или мод требует dependency/library, которой нет среди локально доступных SC2 dependencies, пользовательских модов или установленных game data, присвоить документу статус `SKIPPED_MISSING_DEPENDENCY`, перечислить точные отсутствующие dependency ID/name/path и продолжить очередь. Не скачивать неизвестную библиотеку автоматически, не подменять её похожей, не создавать fake/stub dependency и не считать неполный анализ доказательством безопасной оптимизации. Для такого документа разрешены только read-only inventory и diagnostics; archive write, decor removal, compression claim и Editor PASS запрещены до появления зависимости.

---

# 3. МАТРИЦА РЕАЛЬНОЙ ПРОВЕРКИ

Для каждого найденного документа выполнить минимум:

## A. Read-only baseline

- archive open;
- inventory всех entries;
- Objects/Regions/MapScript/GameData detection;
- dependency inventory;
- dependency resolution result: `RESOLVED` или `SKIPPED_MISSING_DEPENDENCY` с точным списком отсутствующего;
- parse completeness;
- SHA-256 source;
- source size;
- analysis duration;
- peak memory, если доступно без нестабильных hacks;
- список warnings/errors.

## B. Настоящий dry-run

- построить фактический optimization plan;
- показать selected/applied/skipped/blocked;
- доказать, почему каждый destructive candidate разрешён или заблокирован;
- не считать отсутствие parser support доказательством unused/safe.

## C. Настоящая оптимизированная копия

Если анализ Complete и есть доказанно безопасные операции:

- создать отдельный output archive;
- реально применить операции;
- не ограничиваться mocked или in-memory success;
- сохранить before/after hashes entries;
- проверить, что source hash не изменился.

Если безопасных операций нет, это корректный результат `NO_SAFE_GAIN`, а не повод удалять что-либо агрессивнее.

## D. Повторное открытие и re-analysis

- output archive открывается тем же archive backend;
- все обязательные entries читаются;
- XML/Objects/Regions/Galaxy повторно парсятся;
- reference graph перестраивается с нуля;
- запрещены stale cached results;
- нет новых missing strong references;
- нет новых parse errors;
- нет потери неизвестных полей;
- для decor streaming проходит round-trip и outside-scope preservation.

## E. Сравнение результата

Для каждой карты report должен содержать:

```text
source_path
source_sha256
output_path
output_sha256
source_bytes
output_bytes
saved_bytes
saved_percent
analysis_complete_before
analysis_complete_after
parse_errors_before
parse_errors_after
strong_missing_refs_before
strong_missing_refs_after
entries_added
entries_removed
entries_rewritten
objects_removed
galaxy_functions_added
source_unchanged
structural_verification
semantic_verification
editor_acceptance
runtime_acceptance
warnings
limitations
```

---

# 4. EDITOR ORACLE — ЭТО ОБЯЗАТЕЛЬНО

StarCraft II Editor является внешним oracle совместимости.

Для главной миссии Editor acceptance обязателен. Для корпуса выбрать воспроизводимую representative matrix:

- минимум 2 небольших карты;
- минимум 2 средних;
- минимум 2 больших;
- минимум 2 с кириллическими/сложными именами;
- минимум 1 карта с локальной mod dependency;
- минимум 1 `.SC2Mod`, если Editor позволяет корректно открыть этот тип.

Если один файл одновременно покрывает несколько категорий, это разрешено, но итоговая выборка не меньше 8 разных документов плюс обязательная миссия.

Карты со статусом `SKIPPED_MISSING_DEPENDENCY` не ставить в Editor queue и не учитывать как Editor failure приложения: заменить их следующими подходящими документами representative matrix. Если отсутствующая dependency нужна обязательной миссии, зафиксировать `SKIPPED_MISSING_DEPENDENCY`; остальные карты продолжить, но обязательный mission gate остаётся невыполненным и не может быть объявлен PASS.

На каждый Editor run:

1. открывать только копию под `target/diag/beta2-real-maps/editor-oracle`;
2. не сохранять поверх source;
3. дать не более 200 секунд до interactive/editor-ready состояния;
4. проверить alerts, document load errors, dependency errors, trigger/Galaxy validation и placement errors;
5. если лимит превышен — `SKIPPED_EDITOR_TIMEOUT`, зафиксировать evidence и перейти к следующей карте;
6. после timeout разрешено закрыть/restart Editor только после проверки, что несохранённый документ является диагностической копией;
7. не трактовать timeout как PASS;
8. не трактовать зелёный внутренний XML verifier как Editor PASS.

Для главной миссии дополнительно подготовить понятный manual checklist пользователю:

- карта открывается;
- триггеры компилируются;
- миссия запускается;
- основной этаж выглядит как раньше;
- нижняя часть карты выглядит как раньше до вызова generated functions;
- выбранные Region functions создают decor actors;
- clear/restore не создаёт дубликаты;
- повторный create idempotent;
- игровые units/destructibles/trigger-referenced doodads не потеряны.

---

# 5. MAP PERFORMANCE — РЕАЛЬНАЯ REGION DECOR OPTIMIZATION

Сохрани текущую концепцию:

```text
выбрать настоящий Region карты
→ посчитать ObjectDoodad внутри точной geometry
→ показать Preview
→ удалить только доказанно безопасный decor из Objects в КОПИИ
→ сгенерировать Galaxy actor create/clear/restore API
```

Требования:

1. Только реальные Regions из компонента `Regions`.
2. Не возвращать `Entire Map`, `Custom Area` и auto-grid в основной Beta 2 flow.
3. На карте рисовать точные circle/rectangle/polygon/composite shapes, не только bounding rectangles.
4. Координаты Region overlay должны соответствовать реальным map bounds/aspect ratio. Нельзя масштабировать только по min/max найденных doodads, если это сдвигает Region относительно карты.
5. Если точные map bounds не удалось доказанно прочитать из MapInfo/terrain metadata, явно показать `APPROXIMATE_BACKGROUND_ALIGNMENT`, а destructive spatial classification всё равно выполнять в world coordinates, не по пикселям UI.
6. Клик по карте и клик по всей строке Region должны быть эквивалентны.
7. При наложении Regions выбор должен быть детерминированным, а UI должен позволять переключить другой Region из списка.
8. До Preview показывать быстрый candidate count.
9. Preview обязан перечислить:
   - Region и точную geometry;
   - каждый удаляемый Objects entry;
   - каждый static/blocked doodad и причину;
   - generated Galaxy functions;
   - estimated Objects bytes saved;
   - итоговый archive-size estimate с пометкой, что это оценка.
10. Архив создаётся только после успешного preflight. Пользователь не должен ждать несколько минут, чтобы в конце узнать о заранее обнаружимой сильной ссылке или invalid rename.
11. После archive write выполнить fresh re-analysis и отдельный Editor oracle.
12. Полностью убрать из UI, Settings, wizard, tooltips, локализации и обычного пользовательского workflow параметр **`Actors created per game tick` / `Action per tick`**. Generated public Galaxy API также не принимает этот параметр. Если безопасная генерация большого числа actors требует внутреннего yielding/chunking, это автоматическая implementation detail с доказанным default и без пользовательской настройки; она не должна менять семантику create/clear/restore.

---

# 6. НАСТОЯЩИЙ MAP VIEWER НА БАЗЕ SC2 MOD TOOLS

Пользователь имел в виду **Modkit — StarCraft II modding for VS Code** от Talv/текущей команды toolkit. Канонические точки для обязательного аудита:

```text
Installed extension: C:\Users\Vladimir\.vscode\extensions\talv.sc2galaxy-1.10.5
VS Code extension:   talv.sc2galaxy 1.10.5
Core dependency:     plaxtony 1.10.5
Legacy upstream:     https://github.com/Talv/vscode-sc2-galaxy
Current toolkit:     https://github.com/sc2-arcade-watcher/sc2-galaxy-toolkit
Marketplace:         https://marketplace.visualstudio.com/items?itemName=talv.sc2galaxy
Current product:     Modkit — StarCraft II modding for VS Code (pre-release channel)
Required features:   Modkit: Preview SC2Map / Modkit: Preview SC2Map in 3D
```

Локально установленная stable-версия `1.10.5` предшествует новому Modkit viewer и не является достаточным источником renderer. Получи и проинспектируй актуальный **pre-release Modkit** безопасно, не заменяя молча пользовательское расширение: скачай VSIX/source в diagnostics, зафиксируй version, SHA-256, canonical source commit и license. Актуальный Marketplace прямо заявляет viewers карт `.SC2Map`/`.s2ma` и команды `Preview SC2Map`/`Preview SC2Map in 3D`; поэтому «renderer не найден» нельзя объявлять, проверив только старую локальную 1.10.5.

Задача — взять из актуального Modkit реальный map-preview pipeline: определить используемые archive readers, map/terrain decoders, coordinate transforms, shaders/canvas/webview renderer и dependency resolution; затем встроить разрешённые лицензией части через pinned adapter или аккуратно портировать их в C++/Qt с attribution и regression tests. Простое визуальное подражание Modkit, reuse только названия или возврат к собственному bounding-box renderer не считается выполнением требования.

Используй реальные возможности toolkit для SC2 documents, archives, dependencies, catalog/trigger indexing и map metadata там, где они действительно существуют. Если 2D/3D viewer собран из закрытой или отсутствующей в canonical source части, зафиксируй доказательство и реализуй clean-room compatible Qt pipeline из реальных компонентов карты (`MapInfo`, `t3Terrain`, `Minimap`, `Regions`, `Objects`) с визуальным сравнением против Modkit и Galaxy Editor. Это fallback только на случай доказанного license/source blocker, а не способ пропустить исследование viewer.

Production-приложение не должно зависеть от наличия VS Code или от изменяемой папки `%USERPROFILE%\.vscode\extensions`. После license/API audit используй pinned source/library, изолированный adapter либо перенеси только разрешённую минимальную логику с attribution и tests. Не копируй bundled Blizzard data/assets в repository или release.

Запрещено:

- угадывать repository по похожему названию;
- копировать непроверенный код;
- добавлять dependency без license audit;
- выдавать M3 model viewer за полноценный SC2 map viewer;
- встраивать Blizzard assets в repository/release;
- делать сеть обязательной для локального просмотра карты.

Сначала выполнить discovery report:

```text
candidate name
canonical upstream URL
author
license
last release/commit
supported SC2 formats
terrain support
M3 support
DDS/TGA support
MPQ support
Windows/Qt integration cost
security/supply-chain risks
decision: use / adapter / reference only / reject
```

Проверить как минимум следующие классы решений, не предполагая заранее, что их нужно встраивать:

1. собственные текущие readers проекта (`Sc2Archive`, `Objects`, `Regions`, `MapInfo`, terrain metadata);
2. официальный StarCraft II Editor как visual oracle;
3. открытые SC2 map/editor toolkits с terrain/MPQ support;
4. SC2 M3 viewers/loaders только для слоя models/doodads;
5. DDS/TGA decoders для terrain/minimap textures.

Discovery report обязан отдельно ответить: какая точная pre-release версия Modkit проверена; где находится реализация `Preview SC2Map`/`Preview SC2Map in 3D`; какие части viewer реально интегрированы/портированы; какие функции дал Talv/plaxtony/current toolkit; какие части отсутствуют или запрещены лицензией; какая собственная Qt-часть понадобилась. Не выдавай использование названия Modkit за фактическую интеграцию.

## Level A — обязательный для Beta 2

Сделать хороший интерактивный 2D Map Canvas. Текущий чёрный прямоугольник с тонкими контурами Regions и россыпью точек считается **FAILED PREVIEW** и не принимается как карта.

- настоящий визуальный фон карты, а не пустой Graph-style background;
- аппаратно ускоренный viewport с software fallback;
- correct map aspect ratio;
- `Minimap.tga`/другая реальная preview image без растягивания и ложного alignment, если она присутствует и декодируется;
- если minimap отсутствует, raster preview строится из реальных terrain/map данных, а не из bounding box объектов;
- если ни minimap, ни terrain доказанно не декодируются, показывать явный `MAP PREVIEW UNAVAILABLE: <reason>`, а не фальшивую чёрную «карту»;
- точный world-to-screen transform из MapInfo/terrain dimensions с сохранением aspect ratio, origin, Y orientation и letterboxing;
- terrain, playable bounds и camera/map bounds не смешивать;
- world-coordinate ruler/grid;
- exact Regions overlay;
- Objects doodads/units/destructibles layers;
- hover tooltip;
- click selection;
- zoom/pan/fit;
- visibility toggles;
- selected Region highlight;
- candidate/static/blocked decor legend;
- cached static layers, чтобы UI не лагал на 4–10 тысячах объектов.

Regions рисовать поверх читаемой карты полупрозрачной заливкой и чётким outline. Невыбранные Regions не должны превращать terrain в нечитаемую сетку. Выбранный Region, decor candidates, static/blocked entries и объекты вне области имеют разные стабильные цвета. Карта должна быть визуально понятной без чтения списка снизу.

## Level B — только если формат доказан

- terrain height/texture thumbnail из реальных map components;
- cliffs/pathing overlays;
- корректная ориентация Y и map origin;
- golden-image tests на собственных fixtures и визуальное сравнение с Editor.

## Level C — optional, не блокирует Beta 2

- M3 previews для выбранных decor actors;
- texture/material resolution через локальные SC2 assets;
- никакого bundling copyrighted assets;
- отсутствие model/texture не должно блокировать Region optimization.

## Responsive layout — обязательный для Beta 2

Ни одна кнопка, вкладка, подпись или поле Map Performance не может выпадать за правую/нижнюю границу окна, обрезаться либо становиться недоступной из-за размера окна или DPI.

Проверить минимум:

```text
1280x720  @ 100%
1366x768  @ 100% и 125%
1920x1080 @ 100%, 150% и 200%
windowed, maximized и fullscreen
Russian и English UI
```

Требования к компоновке:

- Map Canvas и Region list находятся в корректных Qt layouts/splitters, без ручных абсолютных координат;
- содержимое, которое физически не помещается, получает нормальный `QScrollArea`/model-view scrolling;
- нижняя action bar с Preview/Create остаётся видимой либо предсказуемо переносится в одну/две строки;
- кнопки имеют корректные `sizeHint`/`minimumSizeHint`, не перекрываются и не уходят за viewport;
- длинный переведённый текст переносится или elide-ится с доступным tooltip, но не расширяет окно за экран;
- tab order позволяет клавиатурой добраться до всех действий;
- запрещены фиксированные высоты, из-за которых при 125–200% DPI исчезают controls;
- изменение размера окна во время загрузки/preview не ломает layout и не замораживает UI.

Добавить automated Qt layout smoke test: открыть Map Performance на каждом размере/DPI-профиле, дождаться layout, проверить геометрию всех видимых interactive controls относительно viewport и сохранить screenshots в diagnostics. Любой control outside viewport, overlap главных кнопок или отсутствующая доступная прокрутка — test failure.

---

# 7. КНОПКА «СИЛЬНО СЖАТЬ КАРТУ»

Добавить отдельную понятную операцию:

```text
Maximum Compatible Compression
Сильно сжать карту (совместимо с редактором)
```

Она не должна быть скрытым alias для удаления textures/assets.

Сжатие и удаление unused content — разные операции:

- compression меняет container representation без изменения логического содержимого;
- asset cleanup удаляет данные и требует отдельного proof-based plan.

## Обязательное поведение

1. Всегда создавать новый output archive.
2. Перед долгой записью выполнить preflight:
   - source readable;
   - output path writable;
   - свободное место;
   - archive backend available;
   - entries inventory complete;
   - unsupported encryption/patch/unknown flags detected;
   - current document not stale;
   - predicted temporary-space requirement.
3. Перепаковывать во временный архив.
4. Для каждого совместимого entry выбирать минимальный из доказанно поддерживаемых Editor codecs/flags.
5. Не recompress already-compressed media вслепую, если это увеличивает размер или время без выигрыша.
6. Не удалять textures, sounds, models, localized strings, previews или metadata в compression-only mode.
7. Не менять имена entries, locale/platform metadata и logical bytes entries без отдельного optimization plan.
8. После записи сравнить распакованные logical bytes или semantic representation каждого entry с source.
9. Output должен быть меньше source. Если безопасного выигрыша нет — удалить временный output и вернуть `NO_COMPATIBLE_SIZE_GAIN`, не создавать файл большего размера.
10. После внутренней проверки открыть output через fresh backend и поставить в Editor acceptance queue.
11. Только Editor-accepted strategy может называться `Compatible`.
12. Если часть archive flags неизвестна — блокировать Maximum mode для этой карты или fallback на доказанно безопасный Normal compression с явным сообщением.

## Режимы UI

```text
Normal Save
Maximum Compatible Compression
```

Перед запуском показать:

- source path;
- отдельный output path;
- source size;
- available disk space;
- logical operations: `archive recompression only`;
- что assets не удаляются;
- ожидаемое время как диапазон, если возможно;
- кнопку Cancel.

После завершения показать неблокирующий OperationResult:

```text
Original unchanged
Source bytes
Output bytes
Bytes saved
Percent saved
Structural verification
Logical-entry equality
Editor acceptance: PASS / FAIL / NOT RUN / TIMEOUT
```

Не обещать значительное уменьшение для карты, уже хорошо сжатой MPQ. Нулевой выигрыш является честным допустимым результатом.

---

# 8. PERFORMANCE И ОТЗЫВЧИВОСТЬ

1. Никакого MPQ scan, XML parse, graph build, terrain decode, preview rasterization, compression или verification в UI thread.
2. UI thread выполняет только bounded model updates и painting.
3. Большие результаты добавлять chunked/batched, не тысячами widgets за один event-loop turn.
4. Graph/Map Canvas используют cached layers и GPU viewport, но сохраняют рабочий software fallback.
5. Не создавать по QWidget на каждый doodad.
6. Для больших таблиц использовать model/view, lazy population и virtualized rows.
7. Cancellation проверяется между archive entries и крупными фазами.
8. Закрытие окна/смена карты не оставляет dangling watcher/callback.
9. Прогресс не должен зависать на 94% во время долгого синхронного `refreshPages()`.
10. Разделить post-analysis UI refresh на короткие queued stages и показывать фактическую текущую стадию.

Зафиксировать измерения на малой, средней и самой большой карте корпуса:

```text
analysis wall time
UI longest blocked interval
peak working set
compression time
verification time
preview first-paint time
```

Цель: UI остаётся переносимым, перерисовываемым и позволяет Cancel во время фоновой работы.

---

# 9. FAILURE INJECTION

Добавить тесты минимум для:

- corrupt MPQ;
- truncated Objects;
- malformed Regions;
- unknown Region shape;
- invalid Galaxy generation;
- strong trigger reference to doodad;
- output already exists;
- output path read-only;
- insufficient disk-space simulation;
- cancellation during compression;
- cancellation during verification;
- worker exception;
- source changed during analysis;
- source changed before commit;
- output archive reopens internally but Editor rejects it;
- compression produces larger file;
- missing optional viewer/toolkit dependency;
- OpenGL unavailable and software fallback used.

Для каждого failure доказать:

```text
source hash unchanged
no partial committed output
temporary files cleaned or explicitly quarantined
clear OperationResult
no false SUCCESS/SAFE/VERIFIED
```

---

# 10. BUILD, TEST И COMMIT DISCIPLINE

Следовать `AGENTS.md` проекта.

После каждого логического этапа:

1. focused tests;
2. Release build;
3. diff inspection;
4. relevant real-map run;
5. commit с узким понятным сообщением.

При изменениях generation/archive/verification обязательно выполнить полный штатный test entry point проекта.

Не коммитить:

- карты;
- архивные payload;
- runtime logs;
- screenshots из пользовательских карт;
- `target/diag`;
- portable/installer binaries.

---

# 11. BETA 2 ACCEPTANCE GATES

Beta 2 candidate можно собрать только если:

## Safety

- source hashes всех real-map runs не изменились;
- no false Safe при incomplete analysis;
- no new strong missing references;
- no partial archive commits;
- decor вне выбранных Regions byte/semantic preserved;
- trigger-referenced/gameplay objects остаются static/blocked.

## Real maps

- весь corpus получил baseline + dry-run report;
- карты с отсутствующими dependency получили `SKIPPED_MISSING_DEPENDENCY` и точный список недостающих библиотек, а очередь продолжилась;
- каждая подходящая карта получила реальный optimized-copy attempt;
- outputs прошли fresh re-analysis либо получили конкретный FAIL/BLOCKED;
- обязательная миссия получила decor Region optimized copy;
- Editor oracle выполнен для обязательной миссии и representative matrix;
- все timeout/failures явно перечислены.

## Compression

- кнопка Maximum Compatible Compression отделена от asset deletion;
- logical entries доказанно сохранены;
- output никогда не больше source;
- минимум три разных карты реально стали меньше либо честно показали `NO_COMPATIBLE_SIZE_GAIN`;
- обязательная миссия после сжатия проверена Editor’ом.

## Viewer

- exact Regions визуально совпадают с world-coordinate geometry;
- map bounds/aspect доказаны или alignment помечен approximate;
- вместо чёрной схемы показан реальный minimap/terrain-backed preview; если источник нельзя декодировать, UI честно показывает причину отсутствия preview;
- discovery report доказывает реальное использование Talv/plaxtony/current SC2 Mod Tools либо честно фиксирует отсутствующие в toolkit renderer API;
- выбор по карте и списку синхронизирован;
- большая карта не замораживает UI;
- software fallback работает.

## UI layout

- параметра `Actors created per game tick` / `Action per tick` нет ни в одном пользовательском экране, Settings, wizard или translation catalog;
- все action buttons доступны при 1280x720 и при 200% DPI в проверяемых конфигурациях;
- ни один главный control не выходит за viewport и не перекрывает соседний control;
- layout smoke tests и screenshots приложены к diagnostics;
- русский и английский текст не ломают размеры окна и кнопок.

## Packaging

- portable содержит все Qt runtime DLL, включая Network/OpenGL/OpenGLWidgets;
- запуск проверен с очищенным Qt PATH;
- installer/portable smoke tests проходят;
- версия отображается как `3.0 Beta 2`, а не Stable/Final.

Если хотя бы один обязательный gate не выполнен, release state:

```text
BETA 2 BLOCKED
```

с конкретной причиной.

---

# 12. ФИНАЛЬНЫЙ ОТЧЁТ

Отчёт должен содержать факты, а не общие заявления:

1. commit range;
2. build configuration;
3. discovered real-map manifest;
4. source hashes unchanged proof;
5. per-map analysis result;
6. per-map optimization result;
7. per-map source/output sizes;
8. required mission result;
9. Region/decor removal proof;
10. generated Galaxy validation;
11. fresh re-analysis result;
12. Editor acceptance matrix;
13. compression strategy и codec/flag evidence;
14. viewer/toolkit discovery report;
15. rendering accuracy limitations;
16. responsive-layout matrix и screenshots;
17. proof that `Actors created per game tick` was removed from public UI/workflow;
18. performance measurements;
19. failure-injection results;
20. portable smoke result;
21. elapsed wall-clock time against the 10-hour budget;
22. remaining limitations;
23. exact manual checks left to the user.

Обязательная таблица:

| Map | Source bytes | Output bytes | Saved | Analysis | Semantic verify | Editor | Original unchanged |
|---|---:|---:|---:|---|---|---|---|

И отдельная таблица функций:

| Feature | Implemented | Synthetic tested | Real-map tested | Editor tested | Limitations |
|---|---|---|---|---|---|
| Exact Region selection | | | | | |
| Objects decor removal | | | | | |
| Galaxy actor recreation | | | | | |
| Outside-scope preservation | | | | | |
| Maximum compatible compression | | | | | |
| Map Canvas | | | | | |
| Talv/plaxtony/SC2 Mod Tools data integration | | | | | |
| Responsive controls at required DPI | | | | | |
| Removed per-tick user setting | | | | | |
| GPU rendering/fallback | | | | | |
| Background analysis/UI refresh | | | | | |

---

# 13. НАЧИНАЙ

Теперь последовательно:

1. прочитай `AGENTS.md` и существующую архитектуру;
2. проверь clean/dirty worktree, не затри пользовательские изменения;
3. создай real-map manifest без изменения карт;
4. создай диагностический runner;
5. зафиксируй baseline всего корпуса;
6. исправь найденные safety/compatibility проблемы;
7. проинспектируй установленный Talv SC2Galaxy/plaxtony и current SC2 Mod Tools; зафиксируй реальные доступные API;
8. замени чёрную схему настоящим ModKit-backed Map Canvas минимум обязательного Level A;
9. исправь responsive layout на всей матрице разрешений/DPI и полностью убери пользовательский `Actors created per game tick`;
10. реализуй Maximum Compatible Compression;
11. реально создай оптимизированные копии всех подходящих карт;
12. выполни fresh verification;
13. прогони Editor oracle queue;
14. собери portable `3.0 Beta 2`;
15. подготовь полный evidence report и проверь 10-часовой budget;
16. не публикуй релиз до ручного пользовательского теста.

Главный критерий успеха:

> Инструмент не просто пишет, что оптимизация работает. Он создаёт отдельные оптимизированные копии реальных карт, доказывает сохранность оригинала и содержимого, измеряет реальный выигрыш, проходит повторный анализ и отдельно показывает, принял ли карту настоящий StarCraft II Editor.
