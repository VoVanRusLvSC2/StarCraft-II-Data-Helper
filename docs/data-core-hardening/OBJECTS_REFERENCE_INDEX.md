# Objects reference index and mutation share source spans

The `Objects` reference index now uses the same text-form scanner as rename and
merge. `ObjectUnit` `Type`/`Unit` values resolve only to `CUnit`, and
`ObjectDoodad` `Type`/`Doodad` values resolve only to `CDoodad`. These scoped
carriers produce strong, rewritable references at their actual source line.
Comments produce no references. Other matching tokens, including unrelated
fields and unsupported carrier syntax, produce blocking references rather
than optimistic rewritable ones. XML-form `Objects` remains unsupported and
blocks matching IDs. An incomplete scan records a coverage issue and emits
blocking matches across the entire file, including the unparsed suffix.

This removes the previous independent `Objects` regex and generic whole-file
ID matcher, which could assign a typed placement reference to the wrong
catalog, count comments, and disagree with mutation. The supported grammar
is deliberately narrow; Editor validation of the full `Objects` grammar is
still open. The copied maps and Editor were not modified for this change.

Post-merge verification now reads `Objects` directly with the removed ID set,
so a surviving placement is detected even though its target declaration has
disappeared from the rebuilt index. Unknown matching carriers block the audit;
typed carriers in another catalog do not. The index and verifier decode valid
UTF-8 and UTF-16LE `Objects` content and reject unsupported encodings. This is
still a shared lexical scanner, not an independent Editor-semantic oracle.
