# Render pipeline nodes — research, taxonomy and design

**Status: stages 1, 2, 4, 5 and 6 are built and tested. 3 is under way; 7 and 8
are not.** `docs/PIPELINE_TODO.md` is the working state — this table says which
stages exist, that one says how far each got and what is next.

| stage | what | state |
|---|---|---|
| 1 | The type system — formats, usage kinds, resolution as a divisor, external resources | **built**, `engine.graph.rendergraph` + `engine.graph.pipelinecatalogue` |
| 2 | The catalogue — 45 node kinds with typed, formatted slots | **built**, `engine.graph.pipelinecatalogue` |
| 3 | The executor — `Renderer::Render` runs the graph | **under way**. D00002, and stage 7's other half is downstream of it. The seam is built and tested headlessly (`engine.render.graphrunner`); `shadow` and `surface` are handlers a `PassTable` finds; four passes and the swap itself are left. Until the swap lands, editing a pipeline changes a document and not a frame. |
| 4 | The static checks — nine of the faults in §1.5 | **built**, `engine.graph.pipelinediagnostics`, and marked on the canvas |
| 5 | Authored order and scopes | **built.** The order check is `OutOfOrder` in `engine.graph.pipelinediagnostics`; scopes are `NodeScope` — `Frame`, `World`, `View`, and not `Surface`, for the reason in the TODO. |
| 6 | The access grid | **built**, `engine.graph.pipelineprofile`, drawn by the Pipeline Profile panel |
| 7 | GPU timestamps and upload counters | **half built.** CPU→GPU traffic is counted at every copy — `FrameResult::UploadedBytes` and `Uploads` — and shown on the profile panel. Per-pass timestamps are D00046, blocked on stage 3. |
| 8 | Readbacks — the viewer's image, channel histograms, overdraw | **not built**. D00047. Faults 3, 4 and 9 stay out of reach without it. |

Two findings came out of building it, both from the checker reporting our own
frame and both recorded in place: `window` and `colour` are **external**
resources — they exist outside the graph, which the model had no way to say —
and a `Storage` output could not be sampled by anything, which made every
compute kind in the catalogue an island.

The ask, in the words it arrived in: *extensive; able to change in what order
things happen; up/down scaling; sampling smaller; visibly see what goes between
the CPU and the GPU, what images are uploaded and what operations take place.*

That is three separate features wearing one coat, and they want separating
before anything is built:

1. **A bigger vocabulary** — more node kinds, and a type system that can say
   more than `Colour | Depth | Texture`.
2. **Authorable structure** — order, resolution, and format as things somebody
   sets rather than things the engine decided.
3. **A frame inspector** — the pipeline as *observed*, not as authored: what ran,
   what it cost, what it read, what was uploaded.

(3) is not a node editor at all. It is a second view over the same graph, and
conflating the two is the single biggest design risk here — see §7.

---

## 1. Three systems, three different answers

### 1.1 Unreal — the Render Dependency Graph

RDG is a **whole-frame scheduler**, not an authoring surface. A pass is C++: a
parameter struct plus an execute lambda handed to `FRDGBuilder::AddPass`.
Resources are *virtual* — `CreateTexture` returns a handle with deferred RHI
allocation.

What it derives from the declarations:

- **Barriers and layout transitions**, from the parameter struct alone. Nobody
  writes a transition by hand.
- **Transient allocation with aliasing** — resources whose lifetimes do not
  overlap share memory. This is the feature that makes a fifty-target frame
  affordable.
- **Async compute scheduling** — passes flagged `AsyncCompute` get fences
  inserted automatically against their last graphics producer.
- **Pass culling** — a pass whose output nothing reads does not run.

Pass flags (`ERDGPassFlags`) are the taxonomy that matters: `Raster`, `Compute`,
`AsyncCompute`, `Copy`. Those four say what *kind of work* a node is, which is
a distinction our `NodeCategory` does not yet make and needs to.

Its debugging surface is **RDG Insights** — graph structure, resource lifetimes,
async overlap and culling, captured with `-trace=rdg`.

**The lesson:** Unreal treats the graph as a compiler IR. The declaration is
minimal and everything expensive is *derived*. Our `RenderGraph` already has the
shape (nodes declare reads and writes; `Compile` orders them); what it lacks is
the derived half — lifetimes, aliasing, and culling.

Unreal's actual frame, in order, is the reference list for §4:
PrePass → HZB → Nanite visbuffer → Lumen scene update → BasePass (G-buffer) →
velocities → light grid → shadow depths → shadow projection → lights (shadowed
and not) → SSAO/decals → subsurface → translucency → volumetric fog →
reflection environment → screen-space reflections → post-processing.

### 1.2 Unity — Render Graph and the Render Graph Viewer

Same architecture, different emphasis. Recording stage declares textures;
execution stage issues commands. What Unity's compiler does that is worth
copying:

- **Pass merging into a native render pass**, so tile-based GPUs keep
  attachments in tile memory instead of round-tripping to main memory.
- **Resource deduplication** — two textures with identical properties become
  one.
- **Allocation bounded by lifetime** — memory appears just before the first
  write and is released after the last read.

But the **Render Graph Viewer** is the part this project should steal outright,
because it is the closest existing thing to what was asked for:

- A grid: **passes across the top, resources down the left.**
- At each intersection, an **access block** — green for read, red for write,
  both for read-write, grey for no access, dotted where the resource does not
  exist yet, blank where it has been freed.
- A **merge bar** under passes that were combined, and — critically — the
  viewer states *the reason* passes could **not** be merged.
- Per-resource: size, format, MSAA count, clear state, whether it lives in tile
  memory.
- Per-pass: attachment dimensions, and the **load and store action** for every
  resource it touches.

That grid is a better answer to "see what happens and in what order" than a node
canvas is. A node canvas shows *intent*; the grid shows *consequence*.

### 1.3 Blender — the compositor

The odd one out, and the most relevant to the *editing* half. Blender's
compositor is an actual node graph an artist wires, and its vocabulary is
organised by what a node does to an image:

- **Input** — Render Layers, Image, Movie Clip, Mask, RGB, Value, Texture, Time.
- **Output** — Composite, Viewer, File Output, Split Viewer.
- **Colour** — Mix, Alpha Over, Bright/Contrast, Gamma, Hue/Saturation, Curves,
  Colour Balance, Tonemap, Exposure, Invert, Z Combine.
- **Converter** — Separate/Combine (RGBA, HSVA, XYZ, Colour), Set Alpha, ID
  Mask, Math, Map Range, Map Value, Colour Ramp, Switch View.
- **Filter** — Blur, Bilateral Blur, Directional Blur, Bokeh Blur, Defocus,
  Despeckle, Dilate/Erode, Filter (kernels), Glare, Inpaint, Pixelate, Sun
  Beams, Denoise, Anti-Aliasing, Kuwahara, Posterize.
- **Vector** — Map Range, Normal, Normalize, Vector Blur, Vector Curves.
- **Distort** — **Scale**, **Transform**, Translate, Rotate, Flip, Crop,
  Displace, Corner Pin, Lens Distortion, Movie Distortion, Stabilize 2D,
  Plane Track Deform.
- **Matte** — Keying, Chroma/Colour/Difference/Distance/Luminance Key,
  Cryptomatte, Channel Key, Colour Spill, Double Edge Mask, Box/Ellipse Mask.
- **Tracking** — Plane Track Deform, Stabilize 2D.

Three things Blender does that we do not:

- **Scale, Crop and Transform are first-class nodes.** Resolution is a thing
  that flows along a wire and changes at a node — which is precisely the
  "up/down scaling, sampling smaller" ask.
- **The Viewer node.** Any wire can be previewed by attaching one. There is no
  separate debugger; inspection is a node.
- **Sockets are typed and colour-coded** — yellow colour, grey value, purple
  vector, blue shader. We have this, with three types; it wants many more.

### 1.4 The captured frame

The three screenshots are stills from a full pass-by-pass walkthrough of one
frame of a CryEngine demo, captured in a RenderDoc-family tool. The transcript
gives the whole sequence, and it is worth writing down because **it is the most
detailed description of a real modern frame available to this project**, and
because the commentary running alongside it is a list of the exact faults a good
editor should be able to find on its own.

The frame, in order:

1. Two quads clear parts of a **4096² D32 shadow atlas**; hardware clears for
   scene depth, volumetric fog resources and the motion-vector buffer.
2. **Base pass into deferred G-buffers.** Albedo `RGBA8` (alpha blank), a second
   `RGBA8` of PBR data, normals packed into `RGBA16` with **material tags in the
   alpha** — glass and window materials share one value, concrete a third —
   and `D24_UNORM_S8_UINT` depth.
3. Animated objects last in the base pass, writing motion vectors to `RG16`.
4. A pixel shader copies the 24-bit hardware depth into an `R32_FLOAT` — a
   full-screen triangle, so **~2.7 million pixel-shader invocations at 1080p**
   just to change precision.
5. That `R32` produces a half-resolution (540p) `D24` and a depth-downscaled
   `RGBA16` whose channels hold min/max of each 2×2 and depth-discontinuity
   information; that in turn produces a **135p** `RGBA16` and `D24` for light
   culling.
6. Hi-Z from the hardware depth, then a decal pass, then box-projection decals
   using the `R32` depth and a *copy* of the normal buffer.
7. A rain-projection pass over copies of normals, albedo and PBR.
8. **Software ray tracing in a compute shader** over low-roughness pixels, using
   depth, normals, PBR and an uncompressed PBR texture pool. Four `RGBA16`
   outputs at **760p — about 30 % below native, roughly checkerboard density**.
   Glass is traced *interlaced*: even lines take the exterior reflection, odd
   lines the interior, resolved temporally.
9. A small compute shader prepares tiled lighting parameters; a full-screen
   triangle does tiled deferred shading of the RT resources.
10. Temporal reconstruction against native-resolution G-buffers and accumulation
    buffers; two buffers are kept for next frame; the result is upscaled to a
    native `RGBA16`.
11. SSDO, blurred depth-aware into a spare normal-buffer copy; a 50 µs downsample
    of *albedo* for colour bleeding.
12. Light culling by rasterising light-bound geometry against the downscaled
    `D24` — noted as **faster than the compute-based light culling in other
    pipelines**, and worth investigating.
13. Shadows: a light's range volume is rasterised into the **stencil channel**,
    and the stencil then restricts where the expensive shadow-projection pixel
    shader runs on an `R8` shadow mask.
14. A single large shader does direct lighting and composites SSDO bleed,
    indirect light and the RT reflections into a native `RGBA16`, plus an
    `R11G11B10` for screen-space subsurface scattering.
15. Volumetric fog, transparents, sprites, rain — the rain masked by the previous
    frame's bloom so drops only appear in lit areas.
16. Depth of field at half resolution, near and far, composited.
17. The post-DOF frame blurred to **270p**, then to the **64×64 `RG16` HDR
    luminance target**, then reduced **all the way to 1×1** — this is capture 3.
18. The 270p is recycled and blurred repeatedly until it overwrites the previous
    frame's bloom `R11G11B10`.
19. A LUT built in two draws, then **tone mapping with bloom composited**.
20. **SMAA**: an edge mask into `RG8_UINT`, the stencil recording which pixels the
    first pass touched so the blending pass can skip the rest, a blend mask into
    `RGBA16`, then SMAA 2TX with history in the alpha.
21. Final transfer to `RGBA8` with composite effects and film grain, then UI.

Total: **13.80 ms** in the debugger, against an in-demo median about 30 % faster
— the analyst attributes the gap to debugger overhead on the software RT.

### 1.5 The eleven faults, and the checks that would find them

This is the part that matters most for what we build. Each of these was found by
a human reading a capture for half an hour. **Most of them are derivable from the
graph.**

| # | The fault, as observed | What the tool could check |
|---|---|---|
| 1 | An `R8` target is cleared and never used at all | **Dead resource**: written, never read. Cullable — RDG already does this. |
| 2 | `RGBA8` and `RGBA16` are cleared, then wholly overwritten by a copy | **Wasted clear**: a clear whose every pixel is overwritten before any read. |
| 3 | The albedo target's alpha channel is blank | **Empty channel**: needs the `separate` node, or a readback histogram. |
| 4 | A half-resolution `RGB10A2` output is "a completely blank image" | Same check, whole-target. |
| 5 | The subsurface `R11G11B10` is empty because the scene has no skin | **Conditionally dead**: a pass whose output is uniform. Argues for `Optional`, which we already have. |
| 6 | Normals are in `RGBA16` where `RGB10A2` would do | **Format overspend**: a target whose declared bit depth exceeds what any reader samples. Needs the format type from §3. |
| 7 | The scene is copied into an `R11G11B10` that is **never used again** | Dead resource, as (1). |
| 8 | A group of draws whose resources "have nothing to do with previous or following draws" | **Disconnected subgraph** — trivially visible on a canvas, which is an argument for the canvas. |
| 9 | No depth pre-pass, so **21 % of the most expensive material area is overdrawn**; adding a partial pre-pass gained **7 %, nearly a full millisecond** | **Overdraw**, which needs an overdraw view mode — a readback, not a graph property. |
| 10 | A precision copy done as a full-screen triangle: 2.7 M pixel-shader invocations to move 24-bit depth into 32-bit | **Raster where copy or compute would do.** This is exactly why `Copy`/`Blit`/`Compute` must be distinct node kinds (§3) — the cost is invisible if everything is "a pass". |
| 11 | Pre-pass objects rendered in the *middle* of the base pass rather than last | **Order.** Built as `OutOfOrder` — and building it corrected this row. It assumed our `Compile` sorts silently, as Unreal's and Unity's do; ours *refuses*, with `ReadsBeforeWrite`. So the check is not finding something the compiler misses, it is saying **which pass to move**: the compiler answers with a resource name and stops at the first, and a panel showing `ReadsBeforeWrite (shadow)` has reported a problem and nothing about where. |

Six of the eleven — 1, 2, 5, 6, 7, 8 — are **static properties of the authored
graph**. They need no capture, no timing and no GPU. A pipeline editor that ran
those six checks continuously and marked the offending node would have caught
most of what took a specialist a careful afternoon.

Three more — 3, 4, 9 — need a **readback and a reduction**: is this channel
constant, is this target uniform, how many times was this pixel shaded. That is
the `viewer` node plus a histogram, not a whole profiler.

Only 10 and 11 are judgement calls, and both are visible on the canvas the
moment node kinds distinguish raster from copy and the order is authored rather
than derived.

**This reframes the whole feature.** The ask was "see what happens". The more
valuable half is *the engine telling you what is wrong with what happens* — and
most of that is arithmetic over a graph we already have.

### 1.6 What the frame says about the catalogue

The named passes above are the ground truth for §4, and several are things our
catalogue has no word for at all: a shadow **atlas** with sub-rectangles reserved
per light; **stencil-restricted** shading; a **downsample chain** whose output is
a pyramid rather than an image; **reduction to 1×1**; **history buffers** that
survive between frames; and copies as first-class work.

---

## 2. What atomic has, and the four things it lacks

| | today | needed |
|---|---|---|
| Resource type | `Colour`, `Depth`, `Texture` | pixel format, channel count, bit depth |
| Resolution | `Width`/`Height`, or 0 = "follow the view" | a scale factor, and a `Scale` node |
| Pass kind | `NodeCategory` (presentational only) | `Raster`/`Compute`/`Copy`/`Blit`, which changes what a node *is* |
| Order | declaration order, `Compile` sorts by dependency | authored order, with the dependency sort as a check |
| Observation | none | the access grid |

The first three are additive. The fourth is a new subsystem.

---

## 3. The type system this needs

`ResourceKind` with three values cannot express any of the ask. Replace it with
two orthogonal facts, because they *are* orthogonal:

**Usage** — what a pass may do with it. This is what the wire rule checks.
`ColourTarget`, `DepthTarget`, `Sampled`, `Storage` (UAV/compute-writable),
`Buffer`, `AccelerationStructure`.

**Format** — what is in each pixel. `R8`, `RG8`, `RGBA8`, `RGBA8_SRGB`,
`R16F`, `RG16F`, `RGBA16F`, `R32F`, `RGB10A2`, `R11G11B10F`, `D24S8`, `D32F`,
plus the block-compressed set the screenshots show (`BC1_SRGB`, `BC3`, `BC5`,
`BC7`) for uploaded material textures.

The compatibility rule becomes two questions instead of one:

- Usage: may this be bound there? (Today's rule, kept — a rendered target may be
  sampled; a sampled texture may not be rendered into.)
- Format: does the reader's expectation match, and if not, is the mismatch
  *lossy*? A `RGBA16F` into an `RGBA8` slot is legal and lossy; the editor
  should allow it and **mark the wire**. That is the "empty 8-bit alpha"
  annotation from capture 1, made structural.

**Resolution joins the type.** A resource is `{usage, format, size}` where size
is either absolute (`64×64`) or a **fraction of the view** (`1/1`, `1/2`,
`1/4`). A wire from a half-res output into a full-res input is legal — it will be
sampled — but a *target* binding of mismatched size is not. The editor can
therefore print `1920×1080 RGBA16F` on every wire, which is exactly what the
capture tool prints, and refuse the mismatches before they compile.

---

## 4. The node catalogue

Grouped by what the node *does to the frame*, following Blender's organising
principle rather than Unreal's (which groups by what the code is). Bold entries
are the eight that exist today.

### 4.1 Geometry — passes that rasterise the world
- **`shadow`** — cascade or spot depth from a light.
- **`surface`** — per-surface-camera views (mirrors).
- **`opaque`** — solid geometry.
- **`transparent`** — blended tail, back to front.
- `depth-prepass` — depth only, so the base pass shades once per pixel.
- `gbuffer` — the deferred split of `opaque`: N colour targets plus depth.
- `velocity` — per-pixel motion vectors, for TAA and motion blur.
- `hzb` — hierarchical depth pyramid, for occlusion and SSR.
- `depth-linearise` — hardware depth to a linear `R32F`. Its own kind because it
  is one of the most commonly misimplemented passes in the industry (§1.5,
  fault 10).
- `depth-discontinuity` — min/max and edge information per 2×2, packed into one
  target. The captured frame builds two of these and feeds light culling from
  the smaller.
- `decals` — projected surface modifications after the G-buffer.
- `sky` / `atmosphere` — the background, before or after opaque.
- `particles` / `ribbons` — the existing effects passes, promoted to nodes.

### 4.2 Lighting
- `light-cull` — assign lights to a screen grid or clusters.
- `deferred-lighting` — shade from the G-buffer.
- `shadow-project` — resolve shadow maps into a screen-space mask. The captured
  frame restricts this with the **stencil**, rasterising each light's range
  volume first so the expensive projection shader runs only where the light
  reaches — a technique our node model cannot currently express at all, because
  it has no notion of a stencil-restricted pass.
- `light-bounds` — rasterise light volumes against a downscaled depth target to
  build the light list. Noted in the capture as materially faster than the
  compute-based light culling in comparable pipelines, and worth measuring.
- `ssao` — screen-space ambient occlusion.
- `ssr` — screen-space reflections.
- `ssgi` — screen-space global illumination.
- `volumetrics` — froxel fog.
- `raytrace` — the vendor-agnostic compute path the video is about.

### 4.3 Composite — one image in, one image out
This is where Blender's vocabulary transplants almost unchanged, and where the
"extensive" part of the ask mostly lives.
- **`tonemap`** — HDR to display.
- `exposure` — auto or manual, usually a compute reduction first.
- `bloom` — downsample chain, blur, upsample chain.
- `blur` — gaussian, box, directional, bokeh.
- `dof` — depth of field, near and far.
- `motion-blur` — from the velocity buffer.
- `taa` / `smaa` / `upscale` — temporal resolve, edge anti-aliasing and spatial
  upscale. The captured frame runs SMAA as three nodes — edge mask, blend mask,
  resolve — with the stencil carrying which pixels the first touched so the
  second can skip the rest. Three nodes rather than one is the honest shape.
- `temporal-reconstruct` — accumulate a sub-resolution buffer against history and
  motion vectors. Needs **history resources**: targets that survive between
  frames, which the graph has no word for and which change resource lifetime
  analysis entirely.
- `sharpen`, `chromatic-aberration`, `vignette`, `grain`, `lut`.
- `mix` — two images and a blend mode, which is Blender's `Mix` and the single
  most-used node in any compositor.
- `math` / `map-range` / `curve` — per-channel operations.
- `separate` / `combine` — split a target into channels and put it back. **This
  is what makes "the 8-bit alpha is empty" visible in the editor** rather than
  only in a capture.

### 4.4 Resample — the "up/down scaling, sampling smaller" ask

The captured frame contains six distinct resolutions — 1080p, 760p, 540p, 270p,
135p, 64², 1×1 — reached by seven separate reduction steps. Every one of them is
a node, and the chain from the post-DOF frame down to a 1×1 average luminance is
the single clearest illustration of why resolution has to be part of the type.
- `scale` — to a fraction or an absolute size, with a named filter (point,
  bilinear, catmull-rom, lanczos).
- `downsample-chain` — build a mip pyramid; outputs an array, not one image.
- `upsample-chain` — the other half, with the bloom-style progressive blend.
- `crop` / `pad` — a sub-rectangle, for split-screen and for jittered renders.
- `resolve` — MSAA down to one sample.
- `blit` — a straight copy, which is `Copy` work rather than `Raster` and is
  worth its own kind so the cost shows up honestly. **Fault 10 in §1.5 is
  exactly this**: a precision change done as a full-screen triangle, paying 2.7
  million pixel-shader invocations for what a copy or a compute dispatch does
  for nothing. If `blit` and `dispatch` are different node kinds from `raster`,
  the mistake is visible in the shape of the graph.
- `reduce-chain` — repeated halving to a fixed size, which is the 270p → 64² →
  1×1 luminance chain. One node, not seven, because the intermediate steps are
  not something anybody wires by hand.

### 4.5 Compute
- `dispatch` — a named compute shader, N groups, declared storage bindings.
- `reduce` — parallel reduction to a 1×1 (average luminance is this).
- `prefix-sum`, `sort` — the building blocks a particle or light-cull pass wants.
- `clear` — explicit, because an implicit clear is a cost nobody sees.

### 4.6 Interface and output
- **`overlay`**, **`interface`** — the existing two.
- **`present`** — to the swapchain.
- `capture` — to a file or a texture, which is what `--capture` already does
  outside the graph.
- `viewer` — **Blender's viewer node**, and the highest-value single addition:
  attach it to any wire and the panel shows that image. It is the bridge between
  the editor and the inspector, and it costs one blit.

### 4.7 Input
- `scene-colour`, `scene-depth` — what the previous stage left.
- `texture` — a named asset from the content store. **This is the row in
  capture 2 that reads `BC1_UNORM_SRGB 256×256`** — the uploaded material
  textures — and having it as a node is what makes uploads visible.
- `constant`, `time`, `camera` — parameters as wires rather than as hidden state.

---

## 5. Ordering

Today order is declaration order, and `Compile` topologically sorts by resource
dependency. Two changes:

**Authored order becomes explicit.** A node carries a sequence number, the
canvas can reorder by drag, and `Compile` *checks* the authored order against the
dependency order rather than deriving it. A conflict is a diagnostic naming both
passes — not a silent reshuffle. This matters because two orders can both satisfy
the dependencies and differ enormously in cost (Unity's pass merging is exactly
this: a legal reorder that halves bandwidth).

**Bands become explicit too.** `PerView` is a boolean today. It wants to be a
*scope*: `once-per-frame`, `once-per-world`, `once-per-view`, `once-per-surface`.
The multi-view seam already needs three of those.

---

## 6. Resolution and sampling

Covered by §3's size-in-the-type plus §4.4's nodes, with one addition worth
stating separately: **the editor should print the resolution on the wire.** Not
in a panel, not on hover — on the wire, the way capture 2 prints it beside every
resource. A half-res chain is then visible as a change of label rather than as
something you have to go and check.

---

## 7. Seeing the frame

This is the part that is not a node editor, and building it as one would be the
mistake.

**The authored graph is what should happen. The capture is what did.** They have
different shapes: the authored graph has one `shadow` node; the capture has four
because there are four cascades. The authored graph has a `texture` input; the
capture has eleven bound textures with formats and sizes. A capture is a *list of
executed passes with their bound resources*, and forcing it into the canvas would
lose exactly the detail that makes it useful.

So: a second view, over the same graph, in the shape Unity chose.

**The access grid.** Passes across the top in execution order, resources down the
left in creation order. At each intersection an access block: read, write,
read-write, none, not-yet-created, freed. Under each resource row, its lifetime
as a bar — allocated at first write, freed after last read.

**Per cell**, on selection: the load and store action, and whether the pass could
have been merged with its neighbour — *and if not, why not*. Unity's viewer states
the reason, and that one detail is what turns the tool from a diagram into an
optimisation instrument.

**Per resource**: format, size, MSAA count, whether it is transient (aliased with
something else) or persistent, peak bytes, and a thumbnail.

**Per pass**: the pipeline statistics from capture 1 — VS/PS/CS invocations,
primitives in and out, pixels rendered — and GPU time. `core::FrameGraph` and the
Tracy integration already collect the CPU half; the GPU half needs timestamp
queries around each pass, which SDL_GPU exposes.

**The CPU/GPU boundary**, which was asked for specifically, is three distinct
things and each wants its own row in the grid:

1. **Uploads** — every `SDL_MapGPUTransferBuffer` and copy pass. The renderer
   already knows the byte count per frame; it is not reported anywhere. This is
   "what images are uploaded", and it belongs beside the instance and particle
   buffer traffic that happens every frame.
2. **Readbacks** — the reverse, and the expensive one, because it stalls.
   `--capture` is the only current caller.
3. **Barriers and transitions** — a pass that waits is a pass whose cost is not
   its own work. RDG derives these; if we derive them too, we can show them.

**The rule for all of it:** derived from the graph and from timestamp queries,
never hand-maintained. A second description of the frame is what `DEFERRED.md`
D00016 is about, and this document proposes enough new surface that the rule
needs restating.

---

## 8. Implementation plan

Staged so each lands working, roughly in value order. None of this is small.

**Stage 1 — the type system** (§3). `ResourceUsage` and `ResourceFormat` replace
`ResourceKind`; size becomes absolute-or-fraction. Wire rule becomes usage plus
format, with lossy connections allowed and marked. The editor prints size and
format on every wire. *Touches: `RenderGraph.hpp`, `PipelineCatalogue`,
`PipelineDocument` text format, `nodeview::Editor`, the panel.*

**Stage 2 — the catalogue** (§4). Fill it out. Most kinds are catalogue entries
with no executor behind them yet, which is honest and useful: a pipeline can be
*authored* before it can be *run*, and that is how the standard frame gets
described rather than hard-coded. Add `viewer` early — it pays for itself.

**Stage 3 — the executor** (D00002, §4.3 of the v0.11 plan). `Renderer::Render`
runs the graph through a `NodeRunner` instead of submitting six passes by name.
Until this lands, editing the pipeline changes a document and not a frame — which
is the thing most likely to be misread as the editor being broken. **This is the
highest-value item in the whole list and it is already open.**

Being done in three steps, each keeping the headless capture byte-identical: the
seam (a `PassTable` and a `GraphRunner`, no device in either), then the six pass
bodies moved out of `Render` one at a time, then the swap — after which
`PassOrder()`, the `Pass` enum and `PassRecorder`'s ordering guard are all
deleted, because `Execute` is the ordering and a second one would be the third
description of the frame that D00016 is about.

One thing the extraction turned up that belongs here rather than in the working
notes: **`opaque` and `transparent` are one `SDL_BeginGPURenderPass` and two
nodes.** The graph says they are separable and the renderer has them sharing an
attachment set, a viewport and a light push. That is not a bug in either — it is
Unity's pass merging (§1.2) arrived at by hand, and it is the first place this
project has to say out loud whether the graph describes *nodes* or *render
passes*. It describes nodes; the merge is a property of the frame, and the thing
Unity's viewer prints beside a merge bar is the reason two passes could **not**
be merged.

**Stage 4 — the static checks** (§1.5). Six of the eleven faults found in the
captured frame are properties of the authored graph and need no GPU at all: dead
resources, wasted clears, format overspend, disconnected subgraphs, and passes
whose output is never read. Each becomes a function over `CompiledGraph`
returning a diagnostic naming the node — which means each is a headless test,
and the panel's job is only to draw a warning triangle on the offending box.

**This is the best value in the whole document.** It is a few hundred lines of
arithmetic, it is entirely testable, and it catches most of what took a
specialist an afternoon of reading a capture.

**Stage 5 — ordering and scopes** (§5).

**Stage 6 — the access grid** (§7), read-only over the compiled graph: no
timings, just who reads and writes what and when each resource lives. Pure
derivation from data we already have.

**Stage 7 — instrumentation.** Half done. The upload counters exist and are
measured at the copy rather than derived from a count, so a layout change cannot
make them quietly wrong; they cover the instance buffer, the particles, the
ribbons and the overlay image, which is everything that crosses today.

What is left is **per-pass GPU timestamps**, and they are blocked on the
executor rather than on effort: there is nothing to put a timestamp *around*
until `Renderer::Render` runs the graph pass by pass. Doing it against the six
hard-coded passes would be instrumenting the thing stage 3 deletes.

**Readbacks** — the other half of the original stage 7 — moved to stage 8, where
they belong: they need a path off the GPU that does not exist yet.

**Stage 8 — readbacks.** The `viewer` node's image, channel histograms (which
answer "is this alpha empty"), and an overdraw view. Last, because it is the most
expensive and the least load-bearing — but it is what closes faults 3, 4 and 9.

---

## Sources

- [Render Dependency Graph in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
- [Unreal's Rendering Passes — Unreal Art Optimization](https://unrealartoptimization.github.io/book/profiling/passes/)
- [Introduction to the render graph system in URP](https://docs.unity3d.com/6000.1/Documentation/Manual/urp/render-graph-introduction.html)
- [Render Graph Viewer window reference for URP](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-viewer-reference.html)
- [Analyze a render graph in URP](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-view.html)
- [Compositing — Blender Manual](https://docs.blender.org/manual/en/latest/compositing/index.html)
- [Filter Nodes — Blender Manual](https://docs.blender.org/manual/en/latest/compositing/types/filter/index.html)
- [Render Graphs — Riccardo Loggini](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html)
- [This Vendor Agnostic Ray Tracing Runs 120FPS](https://www.youtube.com/watch?v=yxSrDAOB2xc) — Threat Interactive.
  §1.4 and §1.5 are drawn from the transcript, supplied separately; the frame
  walkthrough runs from about 03:08 to 21:10.
