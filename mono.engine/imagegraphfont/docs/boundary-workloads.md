# Font boundary workloads

`just imagegraphfont-boundary-bench 1` checks the fixtures before running seven
optimized workloads: bitmap and outline coverage, bitmap and outline distance
fields, Unicode Text rendering, artifact encoding and artifact decoding.
The recipe admits one to five measured samples after eight automatic warmups.
Benchmark output stays on stdout.

The bitmap coverage and Unicode Text fixtures compare literal pixels. Outline
metrics use the pinned Inter font's independently inspected SFNT tables.
Distance fields check real decoder geometry and signs, with stable output
fingerprints. These are native decoder checks; they do not establish licensed
Composer or platform font parity.

The pinned bitmap distance rasterizer maps zero distance to 128. Its symmetric
crossbar centre has zero gradient and therefore zero distance. Monochrome
outline coverage emits a blank 1x1 space frame, while outline distance decoding
omits that empty outline. Fixtures retain these different native behaviours.

Artifact checks cover complete owned configuration, profiles, glyph metrics,
request identities, source observations, read grants and refusal preservation.
They include populated prior owners so old and candidate residency is admitted
together.

The workloads use FrameGraph, HeapProfile and Metrics to validate hierarchy,
exclusive allocation totals and actual input, output and backing byte counters.
Reported owner scopes measure the operation; the benchmark wrapper also checks
and records results. Filesystem reads run with a warm cache. One measured sample
proves the instrumentation executes, not a statistical performance conclusion.
