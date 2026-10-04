# Filter workloads

`just imagegraph-source-family-bench 1` verifies and profiles three Kuwahara
variants and six Blobify shape/distance variants. Each persisted graph feeds a
nonuniform 16 by 16 RGBA8 surface into four linked filter rows with radius 2.
Independent source-equation fixtures check every output byte, dimensions,
format and hash. Dimension refusal preserves the prior output.

The optimized `bench` preset records eight warmups and one measured sample
per variant. Each call requires the evaluator, processor, filter, scratch and
image-allocation scopes, plus actual execution, allocation and payload-byte
counters. Frame and exclusive heap accounting must agree without dropped
scopes. One measured sample establishes instrumentation coverage; it does not
establish comparative performance. Raw benchmark output is not saved to files.

The integration passes the full imagegraph suite and fifteen joined CPU
scopes. Metadata receipts live under
`.cache/build/dev/evidence/pixel-composer-2026-10-04/` as
`filter-profiles-102-receipt.json` and
`filter-profiles-102-joined-results.json`. These source-equation CPU fixtures
do not establish licensed reference or GPU parity.
