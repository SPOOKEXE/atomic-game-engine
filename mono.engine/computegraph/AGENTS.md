# computegraph - module invariants

L3, `shared` tier. Links only `Engine::core` and `Engine::graphcore`.

## Typed compute owns its domain

Own buffer and kernel declarations, binding admission, dependency translation,
reference oracles and bounded request/publication contracts. Graphcore owns
topology compilation. Rendering hosts own executable shader reflection, devices,
uploads, actual GPU submission, fences and resource retirement. A reference
oracle is never evidence of GPU execution.

Keep element capacity, logical count and device-produced count separate. Empty
logical buffers retain a legal bounded sentinel allocation. Device-count
consumers use validated capacity for dispatch and bind the count for shader
guards; they never wait for CPU count readback between passes.

## Refusal preserves accepted state

Validate names, types, stride, access, aliases, reflection versions, dispatch
limits and all byte arithmetic before allocating device resources. Preserve the
64 MiB per-buffer and 256 MiB total payload ceilings. More restrictive caller
bounds are permitted. Owner identities are copied stable host-supplied text;
no world, VM or device pointer belongs here.

Request queues are bounded by pending requests, uploads, readbacks and retained
result bytes. Results publish only after the host observes completion and
validates counts, generation and device diagnostics. Cancellation invalidates
publication rights; the rendering host still retires submitted resources behind
their fences. Nothing here runs a background thread or advances a simulation
clock.
