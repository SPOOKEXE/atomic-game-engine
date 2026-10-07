# Source Bake Path

The pinned `node_path_bake.gml` at commit
`b69eca232217360cf1502ef0223523d818606652` consumes Path slot 0, Segment Length
slot 1, Spread Single Path slot 2, Sample Type slot 3 and Output Amount slot 4.
It reads undeclared slot 6 but overwrites that result with the input path's
`loop` property. The declared Loop slot 5 does not control these samples.

Length mode advances distance from zero through source length inclusively.
Amount mode clamps amount to at least two, emits amount plus one samples,
and clamps open input sample ratios to `.999`. Stored progress remains the
unclamped ratio. Closed input appends the first coordinates with progress one.
Zero-length lines are empty. One line is spread only when requested.

Segments retain numeric `[line][point][x,y,progress]` shape and the Float/Vector
source socket domain. A dedicated owned Bake path operation retains every line
and progress value. Native `bake` serialization admits rows before allocation;
progress is not interpreted as a sampling weight. Its length is measured from
sampled straight segments, segment count is the number of stored points, and
signed distance modulo permits negative extrapolation and wraps exact endpoints.
Fresh baked samples use weight one. An uncached reused output buffer preserves its weight. Repeated equivalent
line/distance samples set weight one from the source cache. The runtime owns a
bounded transient memo of numeric modulo distance and line; source string-key
formatting collisions have not been independently verified.

CPU evaluation admits recursive sampler initialization, whole-batch sampling,
output staging and replacement ownership before allocation. Nonfinite samples,
nonpositive Length steps, excessive sample counts and work/byte refusals publish
no results. Negative lengths retain empty open Length rows and signed Amount
sampling. Closed negative Length rows are diagnosed because their source
append reads an undefined first point. Floating Length steps
retain repeated-addition behavior with a bounded extra slot for endpoint drift.

Fixtures cover both sample modes, open/closed endpoints, zero-length lines,
multiline shape, native roundtrip, signed modulo and refusal preservation.
Joined core validation passed in release-tests on 2026-10-07. This is source-derived CPU behavior, not a
claim of licensed executable parity.
