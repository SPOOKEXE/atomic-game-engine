# Transform Image 3D host routes

The native `image.transform_3d` schema and the pinned source
`pc.3_d_transform_image` use the existing owner/name/generation queue. The graph
host consumes borrowed resolved inputs once. It does not re-evaluate upstream
nodes to manufacture a snapshot. Source plane flags are derived request data;
they are not invented source document fields.

The source profile follows pinned commit
`b69eca232217360cf1502ef0223523d818606652`, specifically
`node_3d_transform_image.gml`, `d3d_plane_mesh.gml`, `__node_3d_object.gml`,
`surface_functions.gml` and the `sh_d3d_3d_transform` shader pair:

- A half-unit plane owns two reversed winding/normal parts, four edges and
  independent front/back material images. An absent back image uses the front.
  The back image may have independent dimensions.
- Perspective scales the authored Y scale by the front aspect ratio. Orthographic
  uses a unit view size. The fixed source camera looks from `(0,0,1)` at the
  origin with `(1,0,0)` up. Existing source camera matrix helpers provide the
  left-handed zero-to-one representation used by this SDL backend.
- Source shader UVs repeat with `fract`. Interpolation above Pixel enables
  filtering. Encoded depth uses the source interpolated vertex clip-depth
  varying. Both source output surfaces are RGBA8 UNORM, as specified by the
  default `surface_verify` format. Inputs retain their own numeric formats.
- The raster maps the opposite winding to the back sampler in one two-sided
  draw. Verification of this facing convention, Vulkan framebuffer orientation
  and source depth against actual reference captures remains a device gate.

The native profile keeps its full-unit plane, native projection and numeric
output format. It shares the host transport, not source raster semantics.
Direct native named-texture bindings retain their existing resident queue path.
Chained native graphs use the same explicit host context without adding a
fabricated source catalogue entry or serializing a runtime executor identifier.

Host captures download rendered and encoded-depth channels only after their
existing submission fence completes. Both image channels and the owned typed
mesh become one immutable receipt. Preview evaluation does not synchronously
wait for a readback. Graph consumers receive the typed mesh through normal
receipt validation and can pass it to existing mesh/scene nodes. Captures use
the existing bounded shared renderer observation cache; cancellation retires
queued work immediately and submitted resources after their fence.

Studio reuses its admitted pending evaluation frame, wakeup and capture-name
cancellation lifecycle. Client bindings hold only the evaluated frame while a
capture is pending; their authoritative world clock continues. The next pump
samples the latest frame after completion. Upstream client Lua observations are
retained within this pending generation, so retries reuse the exact authored
node, resolved inputs, input-image hashes, time and seed. Input changes refuse
that immutable observation rather than repeating its capability side effects.
Completion or binding/document/owner retirement releases those observations.

Admission accounts for prior outputs, mesh/material backing, request pixel
capacities, pending receipts, shared cache backing growth and output reservation
before queue mutation. CPU scopes attribute allocations; copy, upload and
readback boundaries count operations and logical bytes. Driver heap commitments
and physical GPU overlap are not inferred from these counters.

The CPU fixtures cover borrowed request ownership, source winding and aspect,
independent back dimensions, numeric native profile, atomic refusal, typed
recorded graph routing, real queue cancellation, no-device provider admission
and pending capability replay. Joined release65 CPU checks pass all six
request cases, both client host/capacity cases, the complete renderer CPU suite
and the selected client suite. They do not claim source raster parity. Source
image arrays require selected processor-row recordings, matching the existing
host boundary; unresolved array inputs refuse explicitly.

The new Transform GPU checks and backend shader compilation must pass in a
fresh matching build before device completion or source parity is claimed.
Approval for the previous three Composer GPU fixtures does not establish this
new Transform raster gate.
