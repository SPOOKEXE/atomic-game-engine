# imagegraph runner invariants

- This is a shared, headless tool. It may depend on `Engine::core` and `Engine::imagegraph` only.
- Tick and frame-range requests pass explicit tick values to the evaluator. An unsupported animation curve returns `UnsupportedExecution`; the runner never silently selects another frame.
- Input size is bounded before parsing. Pixel dimensions and evaluation memory remain bounded by `Engine::imagegraph::Limits`.
- PNG output is straight-alpha RGBA8, deterministic, and contains no timestamps or machine-specific metadata.
- The CLI selects an output by its durable text ID and prints the evaluator's pixel hash after a successful write.
