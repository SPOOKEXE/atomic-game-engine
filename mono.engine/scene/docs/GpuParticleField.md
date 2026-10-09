# Generic device-local particle fields

`GpuParticleField` is a placed `PVInstance`. Its ECS component owns authored
initial conditions and three visual styles. Live particle positions stay on the
GPU and are excluded from saves and replication. `RequestedCount` selects the
existing population presets through 50,000,000; all selected particles are
simulated, while a bounded cohort is drawn in depth slices.

`SetSpawnSamples(buffer)` replaces up to 8192 reusable initial conditions. Each
32-byte little-endian record contains seven float32 values followed by a uint32:

| Offset | Value |
| --- | --- |
| 0, 4, 8 | Local position X, Y, Z |
| 12 | Lifetime in seconds, greater than zero and at most 3600 |
| 16, 20, 24 | Local initial velocity X, Y, Z |
| 28 | Visual group index, 0, 1 or 2 |

An empty buffer clears the population request. Truncated, oversized and invalid
samples are refused without changing the stored definition. `SetLayer(index,
colour, alpha, size, acceleration)` changes one group's linear RGB tint, opacity,
billboard size and world-space constant acceleration. Indices are zero based,
alpha is in 0..1, and size is in 0..64 world units. Both methods return a boolean
and refuse edits to authority-owned replicated objects. Viewer-local predicted
instances remain editable on client hosts. JavaScript uses `ArrayBuffer` for the
sample buffer; Luau uses `buffer`.

`Bounds` is the local half extent used for recycling. Lifetime expiry or leaving
these bounds selects the authored initial condition again. `Seed` fixes the
sample assignment. `Layers` is a three-bit visual mask. The nearest ancestor
`VectorField2D` or `VectorField3D` supplies target velocity with the existing
bounded vector sampling rules; `VelocityResponse` controls the following rate.
With no vector field, particles retain their initial velocity plus acceleration.
Changing samples resets the device population. Changing tint, size or
acceleration preserves live positions.

The durable `Definition` property is an internal bounded encoding of styles and
samples. Authored worlds and ECS snapshots retain it; scripts use the explicit
methods above. Physics, damage, weather formulas and application-specific group
names belong to the world's scripts.
