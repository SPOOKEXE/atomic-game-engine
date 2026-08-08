# Render pipeline nodes — research, taxonomy and design

**Status: research and design. Nothing here is built yet.** What exists today is
`graph::RenderGraph` (compile and order), `graph::PipelineCatalogue` (eight node
kinds, three resource types), `nodeview::Editor` (the canvas) and
`mono.studio/src/Pipelines.cpp` (the panel). This document is what the next
several versions of that are for, and why.

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

### 1.4 What the screenshots add

The three captures are a RenderDoc-family tool over a deferred renderer, and
they are the clearest statement of the ask.

**Capture 1 — the G-buffer.** Four targets at 1920×1080:
`R8G8B8A8_UNORM` ×2, `R16G16B16A16_FLOAT`, `D24_UNORM_S8_UINT`. Annotated:
*"all 4 8-bit contain deferred/PBR data"*, *"empty 8-bit alpha"*, and the 16-bit
target's alpha channel shown carrying real data. Beside it a **pipeline
statistics** panel: CS/DS/GS/HS/PS/VS invocations, pixels rendered, post-clip
primitives, primitive count, vertex count, GPU time elapsed.

The annotation is the point: **a wasted channel is invisible unless the tool
shows you the channels.** An editor that only draws boxes cannot tell you an
8-bit alpha is empty.

**Capture 2 — one draw's resource set.** The `In / Exe / Out` column is the
whole idea: eleven inputs (G-buffer targets at 1920×1080, plus `BC5_SNORM` and
`BC1_UNORM_SRGB` material textures at 256² and 512², an `A8_UNORM` at 512²) and
three outputs. Every row has a **thumbnail**, a **format**, and a **resolution**.

**Capture 3 — the downsample chain.** Inputs at 1920×1080 and 480×270; output
at **64×64**, `R16G16_FLOAT`. 31 µs, 4096 pixels rendered, 3 vertices. This is
"sampling smaller" made visible — and it is only legible because the tool prints
the resolution of every bound resource next to its preview.

**What this means for us:** the unit of inspection is not the pass. It is the
**(pass × resource) pair**, with direction, format, resolution and a preview.
That is Unity's access grid with thumbnails, and it is the target for §7.

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
- `decals` — projected surface modifications after the G-buffer.
- `sky` / `atmosphere` — the background, before or after opaque.
- `particles` / `ribbons` — the existing effects passes, promoted to nodes.

### 4.2 Lighting
- `light-cull` — assign lights to a screen grid or clusters.
- `deferred-lighting` — shade from the G-buffer.
- `shadow-project` — resolve shadow maps into a screen-space mask.
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
- `taa` / `fsr` / `upscale` — temporal resolve and spatial upscale.
- `sharpen`, `chromatic-aberration`, `vignette`, `grain`, `lut`.
- `mix` — two images and a blend mode, which is Blender's `Mix` and the single
  most-used node in any compositor.
- `math` / `map-range` / `curve` — per-channel operations.
- `separate` / `combine` — split a target into channels and put it back. **This
  is what makes "the 8-bit alpha is empty" visible in the editor** rather than
  only in a capture.

### 4.4 Resample — the "up/down scaling, sampling smaller" ask
- `scale` — to a fraction or an absolute size, with a named filter (point,
  bilinear, catmull-rom, lanczos).
- `downsample-chain` — build a mip pyramid; outputs an array, not one image.
- `upsample-chain` — the other half, with the bloom-style progressive blend.
- `crop` / `pad` — a sub-rectangle, for split-screen and for jittered renders.
- `resolve` — MSAA down to one sample.
- `blit` — a straight copy, which is `Copy` work rather than `Raster` and is
  worth its own kind so the cost shows up honestly.

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

**Stage 4 — ordering and scopes** (§5).

**Stage 5 — the access grid** (§7), read-only over the compiled graph: no
timings, just who reads and writes what and when each resource lives. This is
pure derivation from data we already have, and it is most of the value.

**Stage 6 — instrumentation.** GPU timestamps per pass, upload and readback byte
counts, pipeline statistics where SDL_GPU exposes them. The grid gains numbers.

**Stage 7 — resource previews.** Thumbnails in the grid, which needs a readback
path and a place to put it. Last, because it is the most expensive and the least
load-bearing.

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
- [This Vendor Agnostic Ray Tracing Runs 120FPS](https://www.youtube.com/watch?v=yxSrDAOB2xc) — screenshots only; the
  transcript could not be retrieved (every extraction service returned 403/405).
