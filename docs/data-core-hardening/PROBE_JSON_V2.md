# Analysis probe JSON format v2

`SC2CatalogAnalysisProbe` writes one JSON document to the explicit output path
and does not mix progress messages into stdout. `report_format_version` is now
`2`. `unknown_reasons` is a sorted array of `{reason,count}` records rather
than an object keyed by arbitrary diagnostic text. This avoids case-insensitive
key collisions when consuming large-map reports with PowerShell and preserves
distinct original reason spelling. Readers of historical version-1 files must
accept the earlier object shape separately.

`source_fingerprints` lists the captured source path, size and SHA-256 in source
path order. The report also includes the schema and optimization-settings
fingerprints, dependency roots/mappings, active/inactive GameData entries,
diagnostic layer order, collisions, timing, memory and Unknown counts. A
fingerprint records analysis input identity; it does not prove runtime layer
precedence or complete reference coverage.

`editor_observed_scalar_diagnostics` is an additive v2 array. Each record has
`catalog`, `id`, `field`, `value`, `selected_source`, `selected_location`,
`declarations_in_order`, `evidence`, and `runtime_or_safe_proof: false`.
Currently it is emitted only for the direct two-folder `.SC2Mod` `CUnit.LifeMax`
shape reproduced in Editor 5.0.16.97563: one explicit integer value in each
dependency and optionally one in the local Top, with no parent/default/token,
other fields, extra active objects, or transitive edge. The selected value carries its exact source
declaration. This is an Editor Data Module diagnostic; it does not turn the
dependency graph complete or authorize destructive actions. Unsupported shapes
produce an empty array rather than a guessed value.
The two dependency GameData XML files may be selected by an explicit include
manifest or by the observed single-XML folder-mod form without one; the active
component and exact one active object per dependency are still required.

The 2026-09-29 City probe exposed the compatibility defect: its valid JSON
contained reason keys differing only by case, which `ConvertFrom-Json` could
not materialize as a PowerShell object. Fresh v2 Mercs and City reports both
parsed with PowerShell after recompilation. Mercs: 863 objects, 853 Unknown,
zero Safe, 2,928 ms and 165,998,592 peak bytes. City: 12,856 objects,
12,805 Unknown, zero Safe, 41,470 ms and 578,494,464 peak bytes. City had two
located mod archives but four missing transitive dependency edges, so it
remains Partial. The pre-change reports are retained as read-only evidence,
not as accepted v2 artifacts. These single-run timings are not a speed claim.
