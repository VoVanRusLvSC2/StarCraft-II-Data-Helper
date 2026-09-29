# Ordered raw array declarations — 2026-09-28

The active catalog resolver now records each repeated child field, including
nested struct fields, separately along its local class-default/parent/object chain.
Each `ArrayItemDeclaration` is stored once on its source node and keeps the
ordinal among sibling fields, exact XML address and declaration source,
plus the presence and raw spelling of `index`, `removed`, `value` and `Link`.
An object's ordered `arraySourceChain` points at its local default, ancestors
and own declaration without copying their array items into every descendant.
An absent `value` is distinct from `value=""`; two unindexed elements remain
two entries. The XSD-backed field rule is used only to identify multiplicity.

The regression `arrayDeclarationsKeepOrdinalsMarkersAndProvenance` covers a
unique default, parent and child with two unindexed elements, a numeric index,
named index, removal marker, explicit empty value, and nested `Fidget/ChanceArray`.
Existing cleanup remains non-Safe for these arrays. The model records declarations, not an effective
runtime array: append/replace, index matching and reset semantics still need
Editor evidence. It is not a basis for automatic array deletion.

Read-only Release probes on the same copied archives found 5,186 own raw array
declarations across 863 Mercs objects and 54,354 across 12,856 City objects.
Mercs: 2,907 ms, 166,014,976 peak bytes, 853 Unknown and zero Safe. City:
42,150 ms, 576,417,792 peak bytes, 12,805 Unknown and zero Safe. Both analyses
remain Partial because dependencies are unresolved. The first implementation
copied ancestor arrays into descendants and measured 581,058,560 peak bytes
on City; storing each source only once measured 576,344,064 bytes in the first
repeat and 576,417,792 in the instrumented repeat. This controlled structural
change saved about 4.5 MiB in these runs. Single-run time differences and the
older baseline cannot establish the cost of this feature alone. JSON reports
are in the copied-map benchmark directory under `array-raw-*` and
`array-dedup-*`.

Final Release and Debug CTest each passed 5/5 (104.15 s and 298.96 s) with
`SC2DH_TEST_ARCHIVE` set to the actual copied Mercs map. The three archive
tests were also run explicitly in both configurations and passed with zero
skips. Earlier continuations that named `Mercs.SC2Map` pointed to a nonexistent
path; their 5/5 CTest summaries did not prove these optional archive tests ran.
