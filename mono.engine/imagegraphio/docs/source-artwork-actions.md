# Prepared artwork authoring actions

`SourceArtworkEdit` applies the pinned ASE, ORA and Krita layer-generation
callbacks to an owned native document. The caller supplies immutable successful
file content, the exact authored file node, a bound animator replay owner and
explicit current/next authoring revisions. The helper retains no file handle,
provider, device or borrowed replay pointer.

The document and rebound replay owner publish together. Invalid prepared data,
stale authoring, unsupported reused setters, allocation or operation caps leave
both prior outputs unchanged. Studio admits the history transition before
publishing either owner. Its manual actions reuse the existing resumable file
capture path and exact read grants.

Generate Layers follows source content-output connection order and display-name
matching. It reuses the first matching consumer even if it has another node
family. Structural activity is native document membership: the pinned Node
constructor starts active, Node.serialize refuses inactive nodes, and project
serialization includes only active nodes. Native deletion/history removes the
node and its routes. Deleted source tombstones therefore cannot be reused.
Render flags and an Active input are separate controls and do not suppress
reuse. This saved/native projection does not represent a live foreign undo
object that remains connected while inactive; such an observation would need
an explicit caller bridge before claiming that live-object profile.

Reused positions, names, internal names, other controls and instance
bindings remain intact. Source setters edit the shared physical animator at the
captured signed clock, retaining original key provenance. New nodes use native
catalogue constructor defaults and the selected canvas group, with source
relative positions. This constructor profile does not claim to capture another
application's user preference presets. A reused setter whose exact source value
cannot be represented by its native declared type refuses the whole transaction.
The source itself ignores a denied connection and continues later setters; this
native transaction has an explicit supported-domain boundary rather than
partially publishing an invalid graph.

Mapped artwork instances retain their base name even without canvas groups.
Empty layer-name animator arrays keep the constructor's empty-string default
and track settings. The source arrays remain empty; the native projection adds
no physical saved keys or key identities for them.

The shared granted layered-file host maps source ORA `content.layerData` and
Krita `content.layerDat` into the native owned `layerData` array. Names/order and
owned decoded surfaces come from the parsed file; this normalization does not
claim an identical source runtime struct. A real granted Krita archive fixture
exercises the host observation, metadata and layer-generation transaction.

ASE group layers are skipped, but their original positions still affect spacing.
Original duplicate names receive `_1`, `_2` suffixes; suffix collisions are
retained and can cause later layer generation to reuse a just-created node.
The layer-name map is built from raw names before suffix normalization. Its last
duplicate wins. Consequently a newly suffixed layer name may be absent from that
map. The native file host preserves this quirk and refuses such a missing map
lookup without replacing its previous capture. Layers with a cel after frame
zero receive loop=false; saved layer-loop controls otherwise remain intact.

Match Animation Length changes only the total. Missing/null saved endpoints
follow their source getter fallback, while explicit endpoints remain independent.
An explicit end 12 remains saved when an ASE callback reduces the total to 2.
Native playback projection and source playback-step normalization are separate
operations, as described in the imagegraph source timeline profile.

Tags come from the last tag chunk in frame zero. Later-frame tag chunks do not
participate. Import Tags matches the last existing equal region label, or appends
every tag when the caller selects the Shift action. Region endpoints are copied
as saved one-based source values, including reversed endpoints. Colors use the
source packed RGB channels with opaque alpha. No region sorting is introduced.

The declared ASE Generate Layers Trigger input is unused in the pinned script.
Explicit source buttons call these actions; no per-frame trigger evaluation is
invented.

Operation limits charge previous document and replay owners, the immutable
prepared capture, prior result owners, metadata, candidate document backing,
setter tables and rebound replay candidates. Reuse lookup has a cumulative
16,777,216-step cap. ASE name normalization shares a separate 16,777,216
comparison-work quota across layers and tags, charging a comparison plus all
possibly compared bytes for equal-length strings before comparing. The caps
describe these logical owned payloads and tables;
they do not claim to measure full process heap or JSON/vendor allocations.
History has its existing separate serialized residency cap.

The pinned source is commit b69eca232217360cf1502ef0223523d818606652:
`node_ase_file_read.gml` refreshLayers/setFrames/importTags,
`node_ora_file_read.gml` refreshLayers and
`node_krita_file_read.gml` refreshLayers. Native checked PXC projection preserves
unknown project/node/input/key data and validates reimport before publication.
Licensed-application callback/filtering acceptance has not been run.

## Nested transaction admission

The operation reserves external owners, metadata, constructor and edit/event
scratch before cloning the candidate. Both vector backing capacities are
checked against their preadmitted slot counts; actual edit values and string
capacities (including terminators) must fit the reserved scratch. Events borrow
only stable edit values and ports. They are retired after replay completes.

`RebindGroupReplay` receives the remaining cap after external/scratch owners;
its admission includes the candidate document, original replay, new bound
owner and any prior result. `ReplayGroupAnimatorEdits` additionally excludes
the still-live original replay, and admits candidate + bound + edited owners.
The bound owner is retired before projection. `ProjectGroupReplay` admits
candidate + edited + projected documents/owners under the same reduced cap.
Final rebind additionally excludes the old candidate document and admits
projected + edited + final replay. The projected document replaces the
candidate only after these admissions succeed. No nested callee receives the
full operation cap while unrelated retained owners remain live.
