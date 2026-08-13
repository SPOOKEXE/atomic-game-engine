# render — module invariants

L12, `client` tier. Absent from the server binary entirely.

## No SDL GPU type in a public header

`Renderer.hpp` names `struct SDL_Window;` as a forward declaration and nothing
else. Everything else is behind the pimpl in `Renderer.cpp`.

That is what lets a caller hand this module a frame without acquiring a graphics
API, and it is what will let the L9 render graph sit on top of this without
inheriting SDL's vocabulary.

## What crosses into this module is `scene`, and the conversion happens here

`Renderer::Render` takes `scene::DrawInstance` and `scene::Camera`. It used to
take a `render::Instance` and a `render::Camera` of its own, and both are gone:
a `server`-tier host publishes a draw list and a `client`-tier consumer reads
it, so the payload cannot be a type only this tier can name. `v02v03v04.md`
§2.11 is the argument.

Two rules follow, and neither is checkable by the build:

- **The device layout is private and stays private.** `GpuInstance` in
  `Renderer.cpp` is the `mat4` and the RGBA the pipeline binds, and it is built
  straight into the mapped transfer buffer. Promoting it to a public header
  would put a vertex layout back in front of a caller and would be
  `render::Instance` again under a new name.
- **The view-projection comes from `scene::ResolveCamera`.** Building one here
  is what this module did before, and it was a second answer to handedness, clip
  depth and the order of the product — a disagreement that reads as z-fighting
  rather than as a matrix mistake. The aspect ratio is the swapchain's, because
  that is the image the frame is drawn into.

## The design being followed is RENDER_PIPELINE.md

The renderer is a **graph**: a frame declares what it needs produced and in what
order, and the engine works out what to skip, what to reuse from last frame, and
what never needed computing. `RENDER_PIPELINE.md` is that design and it is the
authority for this module. Read it before adding a pass.

**Where decision 11 and RENDER_PIPELINE.md disagree, this module follows
RENDER_PIPELINE.md.** `repo_layout.md` §16 decision 11 says user shaders compile
at cook time with no compiler on the client. §11.9.1 requires the opposite: a
Tier A swap recompiles a `ShaderScript` whose revision changed, while the engine
is running. Its stated reopen condition — "a game needs generated shaders" — is
met by the graph's own design, so the compiler ships. That is why
`ShaderCompiler` exists and why `libshaderc` is a client dependency.

### Its directory names are not ours

`RENDER_PIPELINE.md` was written against the previous engine, whose layout had
no layer stack. Two of its directories map onto three of ours, and getting this
wrong puts the graph runtime at the wrong height with the wrong tier:

| In that document | Here | Why |
|---|---|---|
| `render/` — device primitives, "nothing in it knows what a frame is" | `mono.engine/render/` **[client, L12]** | meshes, images, pipeline building, shader compilation and reflection |
| `renderer/` — graph, nodes, capabilities, caching, executor | split, see below | one directory there, two layers here |
| `renderer/{Graph,NodeDesc,Handle,Capability,Compile,Executor,Cache,Editor}` | `mono.engine/graph/` **[shared, L9]** | the runtime. `repo_layout.md` §9: one graph library, N node sets, and the node families live with their consumers |
| `renderer/nodes/`, `Shadow`, `Frame`, `Pool`, `ShaderCache`, `Device`, `Swapchain` | `mono.engine/render/` **[client, L12]** | the render node set and its device resources |

**Do not create `mono.engine/renderer/`.** It would sit at no defined height and
would have to be `client` tier, which puts the graph runtime out of reach of
physics, replication and audio — the four other consumers decision 12 exists to
serve.

### Where this module actually stands

Stage 0 of §16's twelve. `Renderer::Render` is one instanced opaque pass and one
overlay pass, and it is a placeholder for stage 1's skeleton rather than a first
increment of the graph — it also violates §1's own split by knowing what a frame
is, which the eventual `render/` must not.

Two prerequisites of the design are deliberately absent, and both are recorded
in `docs/DEFERRED.md`: `ecs::ChangeChannel`, which stage 2's per-node cache
invalidation is built on, and `ecs::Column`/`ComponentSet`, which the document
expects nodes to be stored in as rows rather than objects.

So: **do not start building the graph inside this module.** It needs L9 to exist
first, and L9 needs L3's storage. Adding a hand-rolled pass list here in the
meantime is the thing §2 was written against.

## Two shader compilers, and they are not interchangeable

`glslc` compiles `Engine::resources`' `shaders/` during the build. `libshaderc`,
behind `ShaderCompiler`, compiles shaders that do not exist until the engine is
running. Both are the same upstream project and neither replaces the other:

- **A built-in shader failing is a build failure.** That is where it belongs —
  nobody should discover it in a frame. Do not move the built-in `shaders/` to
  runtime compilation to save build time.
- **A user shader failing is a diagnostic string.** It goes back to whoever
  authored it, with the shader's name and a line number, and the engine keeps
  running. Do not make it fatal.

The renderer is a graph, so the second case is not an edge case: a
`ShaderScript` whose revision changed, a swapped antialias pass, a permutation
the demand pass solved for — none of those are known at build time.

**`ShaderLibrary` is the consumer, and it is what closed `D00110` at v0.15.**
Until then `ShaderCompiler` had no caller and `ShaderScript` was a name in this
file with no class behind it — the compiler existed for a case nothing could
reach. `ShaderLibrary::Resolve` is now the one path from a name to a module: a
`ShaderScript` in the world compiled here, else a built-in `glslc` staged at
build time, else a diagnostic. The two compilers above meet in that order and
nowhere else.

**A shader no longer reaches the GPU only through `CreatePipelines`.** That was
true while the pipeline set was fixed, and it is the sentence `D00110` was
written around. `DrawSlots` binds a per-name variant per run now, so adding a
`.frag` to `Engine::resources` is a change in two places: the file, and
`BuiltInShaderNames`. A file added without the second is a shader nothing can
select, which is exactly the trap `resources/AGENTS.md` refuses for meshes and
textures.

**The test that matters is the malformed one.** A compiler that reports success
unconditionally passes every "valid shader compiles" test ever written. If
`ShaderCompiler`'s error path is ever reworked, the assertion to keep is that
invalid input produces a *non-empty* error.

## No shaderc type in a public header either

`ShaderCompiler.hpp` takes a string and returns a `vector<uint32_t>`. That is
the whole reason the wrapper exists — `render`'s consumers must not have to
acquire a compiler API to include a header, any more than they should have to
acquire SDL.

## The built-in shaders belong to `Engine::resources`, and are reached by name

They live in `mono.engine/resources/shaders/`, compile into
`shaderstage/resources/` and stage into `<program>/shaders/resources/`. This
module *binds* them; it does not own them. `resources::Shader("opaque.vert")` is
the only way to name one — the staged directory is spelled once, in that module,
so nothing here carries a path that a rename could leave behind.

**The per-module directory is still what keeps two shader sets apart.** A module
that grows GLSL of its own compiles it under its own name, and reaching sideways
into another module's `shaders/` is as wrong as including its private headers.
What changed in v0.14 is which module the engine's defaults sit in, not that
each set has one owner: `resources` is linked for its files exactly as a library
is linked for its symbols, and a program that does not link it does not stage
them.

Shaders a *game* author writes are a different thing entirely and none of them
are in this repository.

## The debug panels do not share the renderer's state

`Overlay.hpp` and `DebugPanels.hpp` draw pixels into a CPU buffer. That is
deliberate: the panels have to work when the renderer is the thing being
debugged, so they must not depend on its pipelines, its descriptor state or its
frame pacing. One texture upload per frame, and only while a panel is open.

Do not reimplement them over an immediate-mode UI library.

## Six passes are not an architecture either

`Renderer::Render` is a shadow pass, a surface pass, an opaque pass, a
transparent pass, an overlay pass and an interface pass, submitted in that order
by a function that knows all six by name. It is enough to prove the
staged-shader path, the depth buffer, an offscreen target and the swapchain, and
that is all it claims to be.

**The sixth draws nothing this module owns.** `interface` is a
`FrameOverlayHook`, which is what lets `mono.engine/ui` record an editor's
chrome into this frame without Dear ImGui appearing anywhere in the engine. A
game runs five.

**`mono.engine/graph` describes that order and does not execute it.**
`graph::StandardPipeline` is the same six stages as data, and
`Pipeline::Validate` catches the one mistake that matters — a stage reading a
target nothing earlier wrote.

**Keeping the two in step is a check, not a convention.** `render::Pass` and
`PassOrder()` name this module's six in submission order, and
`tests/Passes.cpp` compares them against that pipeline's stage names, in order,
with no device. A seventh stage on one side and not the other fails the build.
`PassRecorder` walks the same list as `Render` submits and refuses to go
backwards, which is the half a headless test cannot see.

**Do not write the count into that test.** This section said "five" until v0.7
added `interface` and then said something false for a release; `tests/Passes.cpp`
compares the two descriptions against each other and neither against a number,
which is why it did not rot with the prose.

**So: enter every pass through `PassRecorder`, and add its stage to
`StandardPipeline` in the same change.** The first is what the check hangs on —
a pass drawn by calling `SDL_BeginGPURenderPass` inline is invisible to all of
the above, and that is the one hole left. See `D00016`.

The render-node system is where passes become nodes and the description becomes
the execution. When it arrives, this class becomes the backend those nodes
compile to — so do not grow the hand-rolled list further in the meantime. Two
competing ways to describe a frame is worse than either.

## The textures this module owns, and what each pass may assume

- **The shadow map** is written by the shadow pass and read by every pass that
  shades. It is `SAMPLER | DEPTH_STENCIL_TARGET`, and both usages are required:
  a depth attachment that is only a target cannot be read, and a shadow map that
  cannot be read is a pass that costs a draw and changes nothing.
- **The surface textures** are written by the surface pass and read by the
  opaque one, both in the same frame and in that order — so a mirror shows
  *this* frame, not the one before it. `SurfaceSlotState::Ready` is what stops
  the first frame sampling whatever the driver handed back.
- **There is a pair per surface index, and the pair is not a recursion trick.**
  Binding a render target as its own sampler is undefined behaviour; writing one
  and binding the other is what makes the surface pass legal. `Surfaces` is
  `MAX_SURFACES` slots, each a `Texture[2]`, a depth buffer and the matrices
  that drew them; a slot is allocated the first time an index renders and kept
  until shutdown.
- **It does give a mirror inside a mirror, one bounce stale, and that changed at
  v0.8.** The exclusion used to be every surface, so no mirror was ever drawn
  into a mirror's texture and there was nothing to be one bounce deep. It is per
  view now — `if (index == self) continue;`, so a pass excludes only the index it
  is rendering *for* — and every other mirror is drawn from the half of its pair
  this frame is not writing, which is the previous frame's image. That staleness
  is what makes the cycle a line: each surface is being rendered for the others,
  so there is no order in which this frame's could be ready first.
- **`Flags.z` is per draw and never per pass.** It means "this draw samples a
  surface texture instead of its own tint". Setting it for the *whole* surface
  pass is what made the floor sample the previous reflection and show the clear
  colour as a black wedge in the pane — found by eye, not by a test. The world
  draws in the surface pass leave it at zero and the mirror runs in that same
  pass set it; both are correct, and the rule is the granularity rather than the
  value. An earlier version of this file said nothing in the surface pass may
  set it, which stopped being true when mirrors began appearing in mirrors.
- **One opaque white texel, bound wherever a real texture is missing.** The
  pipelines declare two fragment samplers and a draw must bind both — an unbound
  sampler is undefined behaviour on several backends where a wrongly bound one
  is merely ignored. The shadow map exists only when something casts and the
  overlay texture only while a panel is open, so a scene of nothing but
  transparent geometry with the panels closed had neither, and the screen pass
  bound no samplers at all and drew anyway. `Impl::FallbackTexture` is a
  resource for the job rather than another texture borrowed for it.

**The shadow and surface passes draw the whole scene; the screen passes draw the
culled set.** A caster outside the camera's frustum still shadows into it and a
mirror shows what is behind the viewer, so culling either to the eye is the
classic version of that bug — shadows that vanish as their casters leave the
screen. That is why the instance buffer holds two ranges rather than one.

## Winding is counter-clockwise seen from outside

`CULLMODE_BACK` plus `FRONTFACE_COUNTER_CLOCKWISE`, so for every triangle
`(v1 - v0) x (v2 - v0)` must point the same way as the face's declared normal.

**Get it backwards and it does not look like a winding bug.** The faces you are
looking at get culled and the ones behind them do not, so a cube renders as an
open box showing its own interior — and as it turns, faces appear and vanish.
That reads as the renderer dropping triangles at random. It shipped that way
once and was diagnosed from a screenshot rather than from the symptom
description.

The geometry lives in `Primitives.hpp` rather than inside `Renderer.cpp` for one
reason: so `tests/Primitives.cpp` can assert this for all twelve triangles
without a GPU. Any mesh added there gets the same check.

## SDL's clip space is Y-up. Do not "fix" it

The reflex when writing Vulkan is to negate `projection[1][1]` after
`glm::perspective`, because Vulkan's NDC Y points down and GLM builds for
OpenGL's. **That is wrong here.** SDL's Vulkan backend submits a
negative-height viewport — `SDL_gpu_vulkan.c`, "Viewport flip for consistency
with other backends" — so what reaches a shader is already Y-up on Vulkan,
Metal and D3D12 alike.

Adding the correction anyway flips the scene *and* the lighting, which presents
as a shading bug rather than an orientation one. It cost an hour the first time.

The same applies in reverse to the overlay: `overlay.vert` maps `v = 0`, the
first row of the image, to clip `y = +1`.

`GLM_FORCE_DEPTH_ZERO_TO_ONE` **is** still required and is set in `core`. Depth
runs 0..1 on every backend SDL's GPU API targets; only the Y convention is
already handled.

## A null window is headless, and every format question goes through one function

`Initialise(nullptr)` is a device with nothing claimed: no swapchain, nothing
presented, no overlay and no interface pass — and the world still drawn, into
the `SceneTarget` the caller passes to `Render`.

That makes the colour format a question with two answers, so it has exactly one
asker: `Impl::ColourFormat`. Every pipeline and the scene target are built
against whatever it returns, and a second call to
`SDL_GetGPUSwapchainTextureFormat` anywhere else is a pipeline built for one
target and bound to another.

**Headless with no scene target draws nothing rather than pretending to.** Every
pass would run and its result would be discarded, which is a caller mistake
worth reporting rather than a state to tolerate.

## A frame is recorded by one thread, and that is a contract

`Render` and `Shutdown` abort when called from a thread other than the one that
called `Initialise`. Draw several viewports one after another; do not record two
frames at once.

This is a design decision rather than a limitation nobody got round to lifting.
The passes share **one command buffer and one device**, so parallel recording
would serialise at submit and buy nothing — and it would cost the property that
makes a viewport correct, which is that the world pass writes a scene target
*before* the interface pass samples it in the same buffer. An editor's imgui
draw lists are recorded before the renderer runs and bind whatever texture
exists at that moment, so the ordering inside one buffer is the whole reason a
docked viewport shows this frame instead of the last one.

**It is checked rather than written down, because this repository has twice
found a claim that had quietly stopped being true.** v0.7 recorded the
sequential-drawing decision in `ROADMAP.md` and enforced nothing; a sentence in
a document is not a constraint, and the failure it guards against — a second
viewport recording from a worker — produces a driver validation error or a frame
of somebody else's geometry, neither of which points back at the thread that
caused it.

There is deliberately **no handoff**, unlike `ecs::Store::BindToCallingThread`.
A store is picked up by a different worker every tick and a device is not, so a
public rebind here would be a seam for exactly the thing the contract forbids.
Revisit when a viewport's *record* is measurable; at the sizes measured it is
not.

## One buffer pair for every mesh, and a name resolves to a range

`MeshTable` packs every registered mesh into a single vertex buffer and a single
index buffer. A buffer pair per mesh would make every change of mesh a rebind,
which a driver cannot batch across; with one pair a mesh is `(firstIndex,
indexCount, vertexOffset)` and switching costs three integers on a draw call
that was going to be issued anyway.

The price is that adding a mesh re-uploads the whole thing, and that is the
right trade while meshes arrive when *content* does — a handful of times over a
session and never inside a frame. **Growth is a full re-upload rather than a
suballocator**: a free list over device memory is a real allocator with real
fragmentation, and the thing it buys is cheap eviction, which nothing does yet.

`MeshTable::Resolve` never returns null. A mesh that has not arrived is the
ordinary state of a streaming game, and an unknown name draws as a cube —
visibly — rather than making the inner loop branch on null.

## `TextureTable::Find` *does* return null, and the asymmetry is deliberate

It stays honest about absence because two callers need to tell "not registered"
from "registered as something": a thumbnail that has not been built and a
particle run whose sheet has not streamed both want null, and neither wants a
picture. A caller that wants a picture asks `Default` or `Missing` for one, and
which of those it asks for is a decision — see the three-way split below.

## The shared sampler's `max_lod` is what turns the mip chain on

`mipmap_mode` has said `LINEAR` since v0.8 and it bought nothing on its own:
`SDL_GPUSamplerCreateInfo` is zero-initialised, and a `max_lod` of zero clamps
every fetch to level zero whatever the texture holds. A chain built, serialised
and uploaded would still have shimmered, with three modules all looking correct.

The clamp is a constant past `assets::MipLevelCount`'s largest answer rather than
a per-texture number, because SDL takes a LOD range rather than a level count —
a per-texture bound would mean a sampler per texture, and one sampler is what
makes a draw call cheap here.

**This module never builds levels.** The filter is `bake::ResizeImage` and
nothing a shipped game links may link `bake` — `bake/AGENTS.md`. A texture that
arrives with one level, including the built-ins and the two markers, uploads with
one and draws as it always did.

## `DrawSlots` splits consecutive runs and must not sort them

Sorting each run by mesh would produce fewer draw calls, and the blended pass
may not have it: that order is back-to-front from the eye, and reordering it is
exactly the transparency bug the sort exists to prevent. One rule for both
passes is worth more than the draw calls — and a rule that held for one pass and
not the other is the kind that gets applied to the wrong one later.

## The model matrix is not rigid, and the normal matrix is not its upper 3x3

`ToGpu` folds the half-extent into the model matrix, so `mat3(model)` scales
normals as well as rotating them. `opaque.vert` corrects for it by scaling the
normal by one over the square of each axis first — three multiplies rather than
an inverse transpose per vertex.

**This was wrong for four versions and invisible**, because an axis-aligned
normal comes out of the wrong matrix pointing the right way and is renormalised
in the fragment. It became visible the moment a sphere or an imported mesh was
scaled unevenly. The comment in the shader that said the transforms were rigid
was describing what the code assumed rather than what it did.

## What is not here yet

- **No mipmaps.** A 2048-pixel sheet minified onto forty pixels shimmers.
  `assets::Texture` is one image and has no place to put the levels, so the fix
  is a format change that should arrive with sampler work rather than ahead of
  it.
- **No filter on the screen pass, and there must not be one.** A surface camera
  filters by tag; the window shows the world. `DrawSlots` takes a filter and
  every screen-pass call site passes zero, deliberately — a filtered window is a
  game where the player sees a group and cannot tell why.

## The fallback texel, the default texture and the marker are three things

`FallbackTexture` is one white texel and exists so a sampler a pipeline declares
is never unbound — undefined behaviour on some backends, a validation error on
others. It is a stand-in for a **binding**.

`DefaultTexture` is what a drawable with no material is *made of*: a real sheet
of white plastic, compiled in, bound in the colour slot whenever `Textures.Find`
answers nothing. `TextureTable::Default` holds it outside the map so no `Add` can
replace it and no `Drop` can release it.

**Conflating the two is the bug this split exists to record.** The colour slot
took the fallback texel and set the shader's "no texture" flag, so every
untextured part in every scene was flat, untextured white — which is what a
seventeen-name `Material` enum was supposed to be fixing and could not, because a
name is not a texture. Do not reach for `FallbackTexture` in the colour slot
again; the shadow and surface slots are where it is still right, because those
are genuinely absent features rather than defaulted ones.

**It is compiled in and it must stay that way.** It is what a part draws with
before any content has streamed, on a machine with no content store, and on the
frame a fetch fails. A default that had to be fetched would be absent in exactly
the cases it exists for.

`MissingTexture` is the third, and it answers a question the other two do not:
a drawable that *named* a texture this table does not hold. The colour slot
therefore resolves three ways, and the distinction between the last two is the
whole reason the marker exists:

| what the drawable says | what the slot gets |
| --- | --- |
| no texture named | `Default` — the plastic. `Material = None` is a finished state. |
| a name, registered | that texture. |
| a name, not registered, **expected** | `Default` — it is on its way. |
| a name, not registered, not expected | `Missing` — the purple checkerboard. Not a finished state. |

`ChooseTexture` is that table as a function, and it is a free one so a suite can
state the rule without a device.

**Drawing the last two the same way is the bug this closes.** An author's typo
and a sheet that never published both rendered as the default material, which
looks exactly like a part somebody deliberately left untextured — so a missing
texture had no symptom at all until somebody noticed the model was the wrong
colour. It is the same split `scene::KeepLoaded` makes for geometry: no mesh
named draws the cube, a mesh named and absent draws nothing.

**The base colour stops applying when the marker is bound**, and that is not an
optimisation to remove. Every other texture in the slot is modulated by the
material's colour — one grey sheet serving a whole palette — but a magenta check
multiplied by a dark red part is a dark pattern that reads as intent. A marker
that can be tinted into looking deliberate is not a marker.

**"Not here yet" and "never coming" used to be the same state here, and that was
`D00107`.** This module knows what it holds and not what is in flight, so a
sheet still streaming wore the marker for the frames it took to arrive — a purple
shimmer across every imported model on a scene load, indistinguishable from forty
misspellings.

**The fix is that the content pump says what it has outstanding**, through
`Renderer::ExpectTexture` and `StopExpectingTexture`, and the marker now means
only *nothing is coming*. Not a timer in this module, and that was never a close
call: a grace period hides a genuinely missing texture for exactly as long as it
hides a streaming one, and with a byte budget in the path there is no N right for
both a small scene and a large one.

**The load-bearing half is the unmark, and it goes on the request *finishing*
rather than on it succeeding.** A host that unmarked only on arrival would leave
a misspelled sheet expected for ever and the marker would never appear for the
one case it exists for. A failure carries no name — `Take` answers nothing — so
`delivery::AssetClient::NameOf` exists for it, and both hosts read the name
*before* taking because a take is what destroys the record.
