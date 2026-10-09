# live ImageGraph plan

status: implementation and focused validation complete; acceptance remains open. The v2 parser, per-world graph owner, Luau API, render cache and Studio paths are implemented. Static 2D ImageGraph and its evidence are in `IMAGEGRAPH_2D.md` and `IMAGEGRAPH_2D_PLAN.md`. Signed headless Vulkan consumer integration, GPU tests, optimized benchmark, server-only build, layer check and real Studio checks passed. Signed ImageLabel color, portal ownership and adopted-texture checks now pass. Strict CI and full-physics CTest remain pending.

The first live layer stays small: seven 2D nodes, ordinary image outputs, no 3D, audio or broad simulation work. Engine code decides graph meaning. The world owns named inputs. Render owns GPU images. Studio edits and previews through those same paths.

## document contract

Version 2 has exactly these root fields: `version`, `nodes`, `outputs`, `parameters` and `bindings`. A node has `id`, `kind`, `inputs`, `position` and `properties`; an output has `name`, `node` and `space`. A parameter is `Parameter{Name, Type, Default}`; `Type` is `number`, `boolean`, `colour` or `string`. A colour default is four RGBA8 integers from 0 to 255. A binding is `Binding{Node, Property, Input}` and connects one named input to one supported property on one node.

Version 1 remains accepted with its current schema and meaning. An unextended static document writes as v1. The writer uses v2 when parameters, bindings, a data source or a linear output require live fields. The reader rejects malformed versions, unknown fields, duplicate names, missing references, type mismatches and ambiguous bindings. It does not coerce values or reinterpret v1 fields. Instance inputs override parameter defaults; a binding reads the resulting named input. No matching input means the node keeps its authored property value.

Parameter names, binding input names and instance keys use tokens of 1 to 128 ASCII characters from `A-Z`, `a-z`, `0-9`, `_`, `-` and `.`, excluding `.` and `..` alone. Static node and output names keep their wider v1 rules; live output references and live cooking require portable output tokens, with a diagnostic for a name that does not fit. Runtime graph names are relative `.aimagegraph` asset names. Reject spaces, controls, non-ASCII, `%`, `?`, `#`, backslash, colon, and `.` or `..` path segments.

Only these 2D node kinds are live:

- `image.source`
- `image.solid`
- `image.resize`
- `image.crop`
- `image.transform`
- `image.flip`
- `image.blend`

Keep the static document's dimensions, bytes and pixel-work limits. `Prepare` checks the selected output's dependency cone against those bounds before GPU work. It does not evaluate pixels or touch a device.

Source JSON has `interpretation: "colour"` or `"data"`. Colour sources normalize to encoded sRGB bytes; data sources preserve raw channel bytes. Graph math operates on stored byte channels and quantizes every intermediate. Output JSON has `space: "srgb"` or `"linear"`. Space declares the byte interpretation and does not transform values. Alpha remains a byte channel. Material numeric maps use data and linear unless their material contract says otherwise. Source interpretation and output space are structural and cannot be bound. `colour` is one RGBA8 property; no `colour.a` binding exists.

## owners and references

The L7 scene ECS carries only plain data in `ImageGraph`: `Graph`, `InstanceKey`, `Output`, `Inputs` and `Revision`. `Graph` names a `.aimagegraph` asset. `InstanceKey` is unique within its world and stable as text. `Inputs` holds typed named overrides. `Revision` is a local invalidation counter; never save or send it. Do not serialize ECS entity IDs.

The world owner is keyed by world plus `InstanceKey`. A direct authored asset output uses `imagegraph://asset.aimagegraph#output`. A world instance uses `imagegraph-instance://key#output`; world scope comes from the caller. Script methods are `SetInput`, `GetInput` and `GetImage`. `GetImage` returns the stable image reference, not CPU pixels.

`.aimagegraph` is a signed opaque asset. Host admission verifies its signature and content hash before parsing. Cooking keeps the live graph as `.aimagegraph`, resolves and hashes exact source dependencies, and writes those dependencies as ordinary `.atex`. Static export still writes the selected output as `.atex`. The CLI flags are `--live-imagegraphs`, `--input`, `--output` and `--only SOURCE`; `--only` selects one source and its live graph texture closure. No recursive graph sources. No ECS number crosses a save or world boundary as identity.

## live GPU path

L12 render owns `Set`, `Evaluate` and `Drop`. `Set` admits a prepared instance and its sources. `Evaluate` executes only dirty nodes in the selected cone and publishes ordinary `TextureTable` outputs. `Drop` releases one world's cached instance. Cache identity includes graph content hash, world-local instance key, output name, effective inputs, revision and dependency hashes.

Changing an input to its existing value is a no-op. A real change marks only that bound node and its descendants dirty. An unchanged frame issues zero GPU commands, uploads and resource creations. Live evaluation never reads GPU pixels back to CPU. Reuse existing render resource accounting and normal deferred texture lifetime.

Updates are transactional. Validate and prepare first. Admit signed sources and check bounds before allocating staged GPU resources. Queue work only after those checks succeed. Swap the ordinary output binding only after allocation and command submission are accepted. On any refusal, discard staged resources, report a useful diagnostic and keep the last-good output. Retire replaced textures through the normal device lifetime path.

Studio exposes parameter values and bindings, previews the selected output on GPU, publishes live graphs and applies outputs to ordinary image slots. Consumers are particles, GUI, all six sky faces and materials. Material numeric slots require explicit data and linear handling. Existing consumers keep loading ordinary images; they do not gain graph-specific branches.

## function contracts

- `Read` bounds and parses v1 or v2 into temporary authored data, validates the schema and graph, then replaces the destination only on success.
- `Write` validates before encoding. Unextended static data stays v1; live inputs, data sources or linear outputs require v2.
- `ResolveInputs` is pure. It combines typed defaults and overrides, applies supported bindings and returns a validated resolved snapshot.
- `Prepare` is pure. It selects one output cone, checks source extents and budgets, then returns an execution plan without evaluating pixels or touching a device.
- `scene::SetImageGraphInput`, `scene::ResetImageGraphInput` and `scene::GetImageGraphInput` manage typed overrides. A changed value advances revision; reset restores the graph default.
- `Content::Admit` accepts host-verified graph bytes, validates portable references and runtime `.atex` sources, then retains the bounded document.
- `Renderer::SetImageGraph`, `Renderer::EvaluateImageGraph` and `Renderer::DropImageGraph` own resident inputs, dirty-cone GPU execution and cleanup. Refusal keeps the prior published output.
- `PublishLiveImageComposer` cooks and signs through the existing asset path; `ApplyImageComposerSelection` assigns the stable instance reference to ordinary image slots.

## implementation sequence

Steps 1 through 9 below are implemented. Step 10 remains open while CI finishes.

1. Freeze v1 behavior and v2 schema with strict parser cases.
2. Add typed parameters and bindings to the authored document.
3. Implement pure `ResolveInputs` and bounded `Prepare`.
4. Add stable per-world `InstanceKey` ownership in scene ECS.
5. Add Luau input and image-reference calls without ECS IDs.
6. Add signed `.aimagegraph` admission and normalized `.atex` cooking.
7. Add L12 source admission, staged allocation and `Set`/`Evaluate`/`Drop`.
8. Add dirty-cone cache keys and last-good transactional output publication.
9. Add Studio input/binding controls, GPU preview and ordinary-slot apply.
10. Run the acceptance gates below; update evidence only from actual results.

## verification results

These focused results passed:

- Signed live integration and independent output fixture: 2 cases and 76 assertions total. `LiveImageGraphContentRender` is 1 case and 61 assertions: Luau changes tint on a signed graph, then checks ImageLabel, particles, `MeshPart.TextureID`, `EmissiveMap` and all six sky faces over 150 frames. The authored sample `[255, 64, 0]` now captures green byte 64 after the `SampledSRGB` boundary change. The missing-source fixture once captured 0 cyan pixels against a threshold above 2,000; it passes after the loader fix.
- GPU renderer: 8 cases and 788 assertions, including literal R8, R16 and R32 data parity and the 1,405-byte output path. Texture GPU: 8 cases and 228 assertions.
- `InterfacePass` target/encoding and portal ownership checks: 2 cases and 1,059 assertions. Capture, adopted-UNORM and texture-owner checks: 7 cases and 230 assertions.
- Composer and dock CPU suites: 22 cases and 469 assertions, excluding asset suites.
- Real Studio Vulkan/SDL/XTest dock check passed two complete undock/resnap cycles. Initial origins were (0, 0) and (300, 100); final dock id was `00000003` (original); process exit was 0.
- Real Composer GPU check changed a named output from 64 to 32 pixels. Save/open retained the v2 binding, default input and authored sRGB output. Setting width to 0 showed a diagnostic and kept last-good preview; reopening restored the saved graph. Publish without a signing key refused without writes; process exit was 0.
- The Studio preview mid-tone bug displayed byte 64 as 13. The preview adapter now uses temporary linear storage while authored and exported data stay sRGB; a CPU fixture covers that path. Successful GUI publish/apply/export was skipped to avoid writing to the default user content store. No user config was touched; CPU and signed live integration checks cover successful publication and apply.
- The Studio preview mid-tone correction keeps temporary linear preview storage separate from authored and exported sRGB. The signed ImageLabel capture verifies the authored byte survives the render boundary.
- Shared Luau and JavaScript API: 1 case and 24 assertions. Client/server property audits: 1 case and 1 assertion.
- Scene: 4 cases and 128 assertions. Replication: 12 cases and 84 assertions.
- Full client retry: 300 cases and 16,091 assertions passed. ImageGraph ASan and UBSan: 30 cases and 963 assertions passed with leak checking enabled.
- Server-only build passed. Architecture: 38 modules, 1 program and 29 layered modules, with no graphics stack. Server reports version `0.26.0`.
- Sequential dev and server physics-suite retries each passed 10 cases and 70 assertions after concurrent preset attempts collided on SQLite. Full dev/server physics CTest runs remain active.
- Component catalogue check passed for 235 components. Module pages check passed for 44 pages.

## measured GPU profile

Five samples ran on an RTX 4090, driver 580.173.02, Vulkan, using the optimized `bench` preset at O3 with heap profiling enabled. This is not the exact shipped release build. Workload was Source -> Transform (bilinear) -> Blend at 256 and 1024 extents. Output stayed on stdout; no benchmark file was written.

| 1024 workload | median wall time | latest GPU timestamp | dispatches |
|---|---:|---:|---:|
| cold composition | 1.150383 ms | 45.312 us | 3 |
| source edit | 1.889402 ms | 42.24 us | 3 |
| transform edit | 1.130765 ms | 29.696 us | 2 |

At both extents, 1,000 unchanged cache hits measured 88 ns per call and recorded zero GPU commands, uploads or device allocations. The 1024 graph retained 16 MiB. A transform edit made zero device allocations. GPU timestamps above are the latest sample, not medians. Wall medians include benchmark-only 1 ms fence polling; the live path uses nonblocking GPU work and does not poll that fence.

## remaining acceptance gates

- The prior strict `just preset=ci check` run ended with SIGTERM 143 and no assertion failure. Its pre-stable rerun was intentionally canceled because it used code from before the latest fixes. Run strict CI against the stable implementation commit; no final result exists yet.
- Full dev and server physics CTest runs remain active. Earlier concurrent preset attempts collided on SQLite; sequential physics-suite retries passed 10 cases and 70 assertions each.
- Keep the v0.26 live roadmap items open until stable-code strict CI and full physics CTest pass. Successful GUI publish/apply/export was intentionally skipped because it targets the default user content store; CPU and signed live integration tests cover those paths.
