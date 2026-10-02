# Recorded builtin random inputs

The host copies observed GameMaker desktop calls. It does not substitute a random
generator. Each record binds the complete authored node, resolved controls, input
image bytes, source processor row and exact signed frame to ordered named draws.

Prepare one non-batched node's controls and image bindings at a graph tick, then
attach the explicitly supplied observations:

```console
assetc --export-graph palette-random.graph --prepare-builtin-random palette \
  --builtin-random-output observed.rng \
  --builtin-random-draw random:0:1:0.1 --builtin-random-draw random:0:1:0.9
```

Replay through either image export consumer:

```console
assetc --export-graph palette-random.graph --graph-output image --output first.png --builtin-random-capture observed.rng
imagegraph --input palette-random.graph --output-id image --output repeat.png --builtin-random-capture observed.rng
```

The draw syntax is `operation:lower:upper:result`. Operation names are `random`,
`irandom`, `irandom_range`, `crand`, `random_get_seed` and `random_range`. Values
must be finite literal numbers. The actual source node consumer verifies ordered
operations and bounds during replay. Changed controls, images, time, missing draws
or mismatched calls fail execution.

Array Sample can replay observed `irandom` calls in recursive row order. Each
call must use lower bound zero and inclusive upper bound `row_length - 1`, with
an integer result. A recording must match every call exactly. Without a matching
recording, Array Sample retains its documented native seeded profile. These
profiles do not establish GameMaker executable parity.

Preparation accepts one source processor row. The core preparation API refuses
batching. Multi-record files may be supplied by a host using the core codec; a
frame range requires an exact recording for every observed source invocation.
Preparation flags require `--export-graph`, one graph tick and an exact recording
destination. They are exclusive with ordinary node execution and authored export.
`--graph-image` and exact host read grants supply upstream inputs as usual.

The reader admits exact regular-file size, configured content policy and previous
capture residency before allocating text. The codec bounds text and coexisting
old/candidate observations. Publication uses the shared exact-path atomic writer.
Invalid preparation preserves prior recordings. Failed replay preserves prior
published images. Capture paths must differ from graph input and image output.

The executable fixture exercises recorded palette cluster initialization feeding
a sampled gradient colour into an 8x8 solid image. It checks equal shared-runner and assetc PNG bytes, changed
pixels for changed observations, exact-frame rejection and atomic refusal of
malformed, unsupported-version and wrong-call recordings. The host suite also
checks actual cluster colours, named draw parsing and bounded file retention.
