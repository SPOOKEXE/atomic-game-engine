# imagegraphfont module invariants

L10, shared. This is the bounded font-byte host and owned font-input artifact
boundary used by client, Studio and exporters. Its dependencies are core,
assets, gui and imagegraph. It must not depend on bake, imagegraphexport,
product libraries, renderer objects, GPU APIs or ambient font services.

## Ownership and capabilities

GraphFontInputs owns immutable configuration and its synchronous provider. It
cannot move or copy while requests borrow it. Replace validates and admits old
and candidate residency before publication; refusal preserves the prior owner
and revision. Bind uses the caller's held playback state and explicit byte cap.

Every filesystem observation requires one exact node/path/role read grant and
ContentPolicy approval. Graph-authored names grant no access. Font bytes use
the gui decoder, with its real metrics and named native coverage or distance
profiles. Source recordings retain their exact owned identity and override
native observations. No ambient fallback or font-family discovery is allowed.

## Limits and publication

Bound request, prior result, provider, candidate, decoded pixels and workspace
before growth. The artifact codec calls bounded native document Read and
preserves the destination on refusal. Registered source observation profiles
must not be presented as licensed shader or platform font parity.
