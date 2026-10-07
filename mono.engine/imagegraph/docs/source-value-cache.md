# Source value cache profile

`pc.cache_value_array` records linked values at absolute integer frame indices.
Its start and stop controls are one-based and inclusive. Negative controls select
the first or last project frame. Unwritten array positions contain numeric zero.
Native history owns each captured value and publishes independent output copies.
The cache records evaluated frames only. A fresh owner's direct seek beyond the
capture window produces empty history; visiting the capture frames populates it.

The source uses `array_clone` from `scripts/array_functions/array_functions.gml`.
That helper recursively copies arrays and returns non-array values unchanged.
Consequently, a source struct or surface handle can retain shared mutable identity.
Native structs and surface resources are immutable owned snapshots. Their captured
contents are supported; mutation through a shared GameMaker handle is not proven
by those snapshots. Exact alias parity requires an explicit source host identity
observation. GPU raster coverage observations do not establish struct identity.

The implementation bounds history frames, recursive payloads and retained bytes
before copying. Public evaluation rejects ticks above the fixed timeline limit;
replay validation rejects oversized or unordered frame indices. The capture window compares the signed fractional clock before indexing. Clocks
outside that window publish retained history without writing, including negative
and fractional clocks. Inside the window, signed and fractional cache writes
return an unsupported-execution diagnostic. The pinned source writes through
`cache[CURRENT_FRAME]` directly; its fractional write coercion still requires
a runtime observation rather than applying the separate read-index profile.

`SourceRouting.cpp` checks native content behavior through compiled graphs,
document round trips, sequential and direct-seek replay, nested arrays, selected
surface ownership, and failure atomicity. Those checks do not claim mutable
GameMaker resource identity parity.
