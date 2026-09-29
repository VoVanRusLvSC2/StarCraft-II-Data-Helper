# Optimization settings revision — 2026-09-28

The analysis records a SHA-256 fingerprint of the effective `optimization/closedProjectMode` and `backup/enabled` settings. `QSettings::sync()` refreshes values before calculating the fingerprint. The shared destructive-operation gate rejects an analysis with a missing or changed fingerprint, and finalization marks an analysis Partial if the settings change while analysis runs. This prevents applying a preview under different closed-project or backup choices. A regression in `analysisCompletenessIsCompleteWhenAllSourcesParse` checks a populated fingerprint and rejection of a stale one.

Validation: focused Release regression passed; full Release CTest 5/5 passed in 98.18 s, full Debug CTest 5/5 passed in 302.21 s. Both full suites used the copied Mercs Episode 2 archive. `git diff --check` had no whitespace errors.

## Commit continuation

The six destructive folder operations now pass the analysis fingerprint into `BackupManager::applyFolderTransaction`. The transaction checks it before backup, before commit, before each file replacement, and after committed validation. A mismatch returns `SourceChanged`; if any files were committed, the transaction attempts a verified rollback and preserves externally modified files under the existing conflict policy. Transactions without an analysis snapshot capture the current settings at entry and enforce consistency throughout their own commit.

Two added regressions check a stale snapshot before backup and a settings change after the first of two writes. The latter uses an isolated temporary INI settings store and confirms rollback restores both source files. Full Release CTest: 5/5 in 106.05 s; full Debug CTest: 5/5 in 316.95 s. Both used a copied Mercs Episode 2 archive.

Scope: this narrows the settings race but does not provide a cross-process lock or an atomic multi-file commit against arbitrary external writers. The latest ZIP predates this continuation. Editor behavior, runtime dependency precedence, arrays/index/removed, and the rest of the full assignment remain open.
