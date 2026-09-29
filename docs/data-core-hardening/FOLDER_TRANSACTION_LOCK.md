# Folder transaction lock — 2026-09-28

`BackupManager::applyFolderTransaction` now acquires a `QLockFile` before reading source revisions or creating a backup and holds it through validation, commit and any rollback. The lock name is SHA-256 of the case-folded canonical folder path under the process temp directory, so two instances using the same canonical folder and temp directory request the same lock without adding a file to the map. A busy or unavailable lock stops the transaction before any source write. A one-hour stale timeout covers ordinary large saves while retaining Qt's crashed-process lock recovery.

A regression starts a nested competing transaction during staged validation. The competitor is refused without a backup or source edit; after the first transaction exits, the same operation succeeds. Focused Release passed. Full Release CTest: 5/5 in 101.62 s; Debug CTest: 5/5 in 290.68 s, both with the copied Mercs archive. `git diff --check` has no whitespace errors.

The lock serializes cooperating Data Helper folder transactions. Qt only guarantees serialization when processes use the same lock path; external editors and tools do not participate. The existing per-file hash checks and conflict-aware rollback remain necessary. Atomicity across arbitrary external writers and archive-specific write paths remains unproven. Editor runtime behavior and the remaining full assignment are still open. The current portable ZIP predates this lock.

Qt API reference: https://doc.qt.io/qt-6/qlockfile.html
