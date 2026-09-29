# Diagnostic dependency closure and proposed layer order

`dependencySourcesLocated` now reports whether every declared direct and
transitive dependency resolves to a unique readable archive with an active
GameData component. It is separate from `dependencyGraphComplete`, analysis
completeness and destructive permission. A closed source graph alone does not
prove StarCraft II's runtime override rules.

For a closed graph, `hypotheticalLayerOrder` lists each unique dependency after
its own dependencies, retaining sibling declaration order, followed by the
local source. For example, Map -> A -> B produces B, A, Map. This is a proposed
inspection order only. It is not used for effective values, Safe candidates,
rename/merge preflight or apply. Missing, ambiguous, cyclic, malformed or
inactive dependencies yield no proposed order.

The layered declaration index now preserves that diagnostic order when it is
available, and otherwise preserves source discovery order. It no longer sorts
layers by archive filename. A fixture declares Z before A and verifies that
the index retains Z, A, local; the transitive and cyclic fixtures verify the
other boundaries. Focused Release checks passed 5/5 QtTest functions (including
setup/cleanup), zero skipped. A full Debug/Editor rerun is deferred.

`SC2CatalogAnalysisProbe` emits `hypothetical_overrides`: every cross-source
identity collision, sorted by identity, with each raw declaration's class,
original ID spelling, source, XML location and dependency-layer index. The
`declarations_in_index_order` array follows the proposed order only when
`hypothetical_layer_order` is present; otherwise it is discovery order. Neither
array declares a runtime winner. The Release probe target compiles with this
output; no Editor-saved override fixture has been compared to it.

The boundary is intentional: a theoretical overlay can be implemented on top
of this order and labelled as an assumption, but enabling destructive changes
from it requires Editor evidence. The source graph and runtime semantics are
reported separately so that a located archive is no longer confused with a
proven effective catalog value.
