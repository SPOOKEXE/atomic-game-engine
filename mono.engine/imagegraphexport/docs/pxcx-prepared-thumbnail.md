# Explicit prepared PXC thumbnails

Default source-backed saves retain the source THMB and exact no-op archive
bytes. `WritePxcxPreparedThumbnail` is an explicit additional in-memory step
for a caller that has already completed projection serialization and holds
its checked archive plus a matching immutable preview. The caller publishes
and adopts only the final successful candidate. This helper does not evaluate
a graph, open paths, acquire a device or modify the caller's archive/preview.

The source pin is `b69eca232217360cf1502ef0223523d818606652`,
`scripts/save_function/save_function.gml`, lines 108 to 140. Source saving creates
a square surface, scales by `size / min(width,height)`, centers the drawn image
and uses override blending before compressing raw surface bytes. The native
profile uses the established PXCX 256-square RGBA8 thumbnail contract. Pixel
centres select nearest texels with the exact cover-crop geometry. Finite samples
are converted to straight RGBA8 with the existing native surface codec, retaining
alpha without black compositing. HDR samples clamp to this format's range.

This is a native sampling/conversion profile. It does not establish equality
with GameMaker texture-page filtering, fractional edge coverage, alternate
thumbnail preferences, GPU colour conversion or licensed application acceptance.

Graph JSON, opaque META bytes and existing source facts pass through the checked
bake archive writer unchanged. Its container validation occurs before output is
replaced. A repeated explicit update that yields the existing thumbnail retains
exact original archive bytes. Importing the final archive gives the host its new
reference preview and permits subsequent source-preserving no-op saves.

Admission charges the borrowed archive's complete retained capacities, the
preview pixel backing, previous output backing, fixed RGBA pixels, archive clone
and validation slots, and three conservative encoded slots bounded by bake's
compressed graph/thumbnail/META ceilings. The encoded reservation alone is
about 99.2 MiB; the default total operation cap is 128 MiB. Larger retained inputs
can therefore refuse even when their final compressed output is small. No
allocation is permitted to discard the prior output to create headroom.

These are conservative logical payload/staging charges. They are not an
allocator heap measurement. Bake's JSON DOM, compression and decompression
vendor workspaces remain governed by its independent format ceilings. The
operation reports crop pixel count, actual encoded bytes and its admitted
payload reservation through existing Engine profiling and metrics.

Focused fixtures use independent literal landscape, portrait and odd-offset
crop pixels; straight HDR/alpha conversion; real projection edit, reload,
reimport and no-op adoption; and malformed layout, nonfinite samples, stale META,
missing serialized provenance and retained-capacity/budget refusal.
