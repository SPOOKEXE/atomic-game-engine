# Native HLSL annotation in retained PXCX

This envelope belongs to Atomic Game Engine. It is not a Pixel Composer socket,
attribute, or pinned source serialization field. Official application retention
has not been verified and is not claimed. Original archive bytes remain the
no-op publication result.

A semantically mapped `Node_HLSL` source node may contain:

```json
"atomic_game_engine": {
  "version": 1,
  "composer_cooked_shader": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.ashader"
}
```

The selector projects to `pc.hlsl` `Node.SourceProperties` with the exact port
`composer_cooked_shader` and a string value. The envelope has native format 9
semantics, including when only its version or unknown future fields remain.
The first 64 characters are lowercase hexadecimal; the total name length is 72.
The canonical ASH1 content BLAKE3 identity is established by the renderer's cook
owner, not recomputed by the import adapter. Importing a well-formed name does
not establish that its asset exists or matches the authored shader controls.
No shader bytes, file paths, providers, runtime pointers, or compile commands
are saved here.

Unknown envelope members and all upstream node/project fields are preserved.
Insertion creates the namespace only when absent. Replacement changes only the
selector. Removal erases only the selector and retains version and unknown
fields. The adapter never creates `attri.composer_cooked_shader`. Incompatible
versions, namespace collisions, malformed names, duplicate native properties,
and reserved selectors on unmapped or non-HLSL nodes fail atomically.

Schema provenance is engine-owned. Foreign node identity and controls are
pinned to Pixel Composer source commit
`b69eca232217360cf1502ef0223523d818606652`,
`scripts/node_hlsl/node_hlsl.gml`: constructor line 41, fixed inputs lines 48..52,
argument definitions lines 59..64. Dynamic HLSL argument type projection is a separate
known importer seam and is not widened by this envelope change.

Checked archive edit/reimport/native-save tests cover
insertion, replacement, removal, unrelated shader edits, unknown field retention,
malformed selectors, incompatible versions, conflicting namespaces and preserved
caller outputs. Joined release56 verifies 72 assertions in both annotation cases.
Official-application compatibility and rendered shader execution remain open.
