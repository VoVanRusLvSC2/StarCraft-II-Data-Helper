# Shared merge reference preflight - 2026-09-28

Merge preview and batch selection now use the shared UnifiedReferenceIndex to reject Strong/Blocking references whose carriers cannot be rewritten. The target catalog scopes the check; a reference without an established catalog also blocks the matching identity. Single apply already requires a valid preview. GUI-specific unresolved checks remain as additional coverage.

The regression galaxyUnresolvedReferencesBlockMerge exercises computed constant concatenation, a dynamic UnitCreate argument, and an opaque unknown-consumer string. Each case verifies preview rejection, single and batch rejection/skipping, and exact unchanged catalog/script bytes. Before the fix the first computed case produced a valid preview. Positive Galaxy and GUI merges still rewrite established values and preserve display/comment bytes. Binary blocking keeps its specific not-safe-text diagnostic and source filename.

Evidence: galaxy-merge-before.txt and galaxy-merge-after.txt. Final Release: 5/5 CTest, 143 core passed / 0 failed / 4 unavailable fixture skips (82.19 s). Final Debug: 5/5 CTest, 143 core passed / 0 failed / 4 unavailable fixture skips (235.49 s). Actual archive tests used the Mercs Episode 2 copy.

A suspected partial-selection issue was investigated and not confirmed: preview.valid is false whenever warnings exist, so single apply does not write a partially rejected request. The exploratory fixture was removed rather than recorded as a proven defect.

Remaining scope: this preflight does not prove typed XML rewriting, cross-catalog redirects, effective dependency/default resolution or independent post-write semantic verification. The previously delivered portable package is an earlier checkpoint and does not contain this change. Editor acceptance remains NOT_RUN. The full assignment is incomplete.
