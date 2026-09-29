# GUI reference spans: implementation and evidence

The registry reads actual TriggerData documents (Standard and Library scopes), ParamDef/ParameterType, FunctionDef/Parameter, FunctionCall/FunctionDef/Parameter, Variable/VariableType/InitialValue, and Param/ParameterDef/ValueType/ValueGameType. Definition identities are library + element ID. Catalog domains use the structural XSD catalog index. Conflicting or missing definitions and unresolved function contexts become non-rewritable references; ordinary established string values do not become catalog references.

Analysis and rename share the registry; merge preview, merge apply and batch merge now use it too. Replacements use pugixml source offsets in the original UTF-8 byte buffer. Only an established gamelink text carrier is rewritten. The entire source buffer must match the registry snapshot. XML entity spelling in the replaced carrier is allowed; every surrounding byte is retained. GUI documents named .xml bypass the legacy catalog XML rewrite loop. Changed GUI outputs are parsed in transaction validation. Unknown GUI references block removal/redirect in their inferred catalog, or all matching identities when the catalog conflicts.

## Reproduced failures

- `hardening-stage/gui/before-tests.txt`: ParamDef-typed link was not Strong; catalog-scoped rename did not produce selected work (2 failures).
- `hardening-stage/gui/merge-before.txt`: merge changed ordinary GUI text/comments; exact byte comparison failed.
- `hardening-stage/gui/unknown-merge-before.txt`: merge preview accepted a source with a missing ParamDef. This regression was recorded before adding the merge preflight.

Logs are in the sibling writable workspace E:/SK2/SC2_Edition_MCP, not embedded binaries or user maps.

## Regression coverage

- guiGamelinkUsesParamDefinitionCatalog: Unit/Actor with the same ID; only Unit is rooted by the gamelink.
- guiRenameOnlyChangesEstablishedGamelinkValues: only the typed Value span changes; display, comment and Actor identity remain exact.
- guiMergePreservesDisplayAndComments: single merge of Triggers and batch merge of Gui.xml, including an entity-encoded ID.
- guiUnresolvedTypeBlocksRename: missing ParamDef or conflicting GameType blocks apply without changing files; an independently known Actor remains Safe in the scoped missing-definition case.
- guiUnresolvedTypeBlocksMerge: preview and single/batch apply cannot delete the target of an unresolved GUI reference.
- mergeRewritesNonXmlReferenceFiles: preserved assertions; the former invented <Trigger><Param> fixture was replaced with real TriggerData ParamDef/Param syntax.

## Remaining requirements

At this earlier phase stock NativeLib bindings were not shipped. They are now shipped and verified as described in GUI_NATIVE_TYPES.md; the remaining paragraph records the earlier stage. A draft extracted from the supplied build-97563 NativeLib has 6554 ParamDefs and 3196 FunctionDefs. Source SHA256: cbb2302825dce57ff9bb8efd44f9b0ed5cb056eee00265e5b711b0824d872396. It independently establishes UnitCreate FunctionDef Ntve|6C39A0DF and Unit gamelink ParamDef Ntve|EF0CF6FF. The draft is only preparatory evidence, not a runtime resource. Calls requiring external definitions remain Unknown until supported source bindings are loaded. Unsupported GUI grammar, non-UTF-8 mutations and unresolved dynamic values remain conservative. Full dependency layers and independent output semantic verification are still incomplete. Editor acceptance: NOT_RUN. The prior Galaxy portable artifact predates these changes.

## Full checks

Final Release: 4/4 CTest PASS; core 136 passed, 0 failed, 4 skipped, 74.856 s; CTest total 91.29 s. SC2DH_TEST_ARCHIVE points to the Mercs Episode 2 copy in the sibling hardening-stage/benchmark folder. The four unavailable fixture skips remain unverified. Evidence: core-tests-Release-gui-spans.txt and gui-full-release-verified.txt. Final Debug: 4/4 CTest PASS; core 136 passed, 0 failed, 4 skipped, 236.190 s; CTest total 246.01 s. Evidence: core-tests-Debug-gui-spans.txt and gui-full-debug-verified.txt. Both final builds include the strengthened entity-only reference regressions.

Additional failing evidence: gui-entity-before.txt (GUI merge skipped the only entity-encoded ID) and gui-rename-entity-before.txt (rename changed the declaration but skipped its only entity-encoded GUI link). Both byte-token gates now defer to the typed GUI registry rather than raw ID spelling.

Next concrete step: ship and validate the offline NativeLib type snapshot with a version/provenance contract, exercise cross-library definitions and actual Ntve FunctionCall parameters, then continue dependency/effective-values and the remaining mutation/transaction work. Draft generator and verified source snapshot are in hardening-stage/gui; they are not yet runtime resources.
