# Pipeline work — progress and what is left

Working list for `docs/PIPELINE_NODES.md`. **That document is the design; this
one is the state.** Kept separately so the design does not have to be edited
every time a box is ticked, and so a session that runs out of room hands over
something a fresh one can pick up without re-deriving anything.

Update this **as work lands**, not at the end. A session that ends mid-stage
should leave the in-progress item marked with what compiles and what does not.

---

## State

| stage | what | state |
|---|---|---|
| 1 | Type system — formats, usage kinds, divisor, external resources | **done** |
| 2 | Catalogue — 59 node kinds, typed and formatted slots | **done** |
| 3 | Executor — `Renderer::Render` runs the graph | **done.** `graph::Execute` is what submits the frame; `PassOrder()` is a view of the graph and the `Pass` enum is gone |
| 4 | Static checks — ten fault kinds | **done.** The tenth is `SamplesOwnTarget`, which became reachable when `raster` let a pipeline wire one |
| 5 | Authored order, and scopes | **done** |
| 6 | Access grid / profiler visualiser | **done** |
| 7 | Instrumentation | uploads **done**; debug groups **done**; GPU timestamps **cannot be built on SDL_GPU** — no such API. See below |
| 8 | Readbacks — viewer image, histograms, overdraw | **done.** Histograms, the download policy, the non-stalling device path, the `viewer` node, the `overdraw` counter, and `Renderer::SetPipeline` so a pipeline with either in it can run |
| 9 | Entity flow, cameras, per-camera pipelines, custom shaders | **working.** `Entities` and `Camera` resources, `world`/`camera`/`light-camera` sources, per-camera pipelines, node parameters editable on the canvas, graph-allocated targets, `raster`/`dispatch` running named shaders, and a camera range per wired list |

---

## Done: stage 5, scopes

**Three scopes, not four.** `Frame`, `World`, `View`. `Surface` is deliberately
absent — the surface pass loops *inside* a per-view one and nothing schedules per
surface, so a fourth value would be a word in the type with no block to run in.

The distinction was **already live in the executor and missing from the
vocabulary**: `RenderGraph::Execute` runs the shared block once per *distinct
world* and the final block once for the frame. A boolean could only say "not per
view", which is two different things.

- [x] `NodeScope`, `Describe(NodeScope)`, `RunsPerView(NodeScope)`
- [x] `Node::PerView` → `Node::Scope`; `Compile`'s partition turns on the predicate
- [x] `Edit::Scope` and the document's text format word — `node "x" "k" world no`
- [x] `NodeKindSpec::Scope`, and the 47 catalogue rows mapped
- [x] `nodeview::ToDocument`, and the suites
- [x] Cases: a `World` pass runs once for two views of one world and **twice for
      two worlds**; a `Frame` pass runs once regardless; the standard frame's
      scopes match its blocks; the three names are distinct

**Two mistakes the tests caught**, both from a blanket rename:

- `CompiledGraph::PerView` is a *vector of node ids*, not the node field. A
  find-and-replace turned an assertion about it into nonsense.
- Every non-per-view node became `World`, including the overlay and the chrome.
  Those are per **frame** — they are drawn once over whatever the worlds
  produced. `the standard frame's scopes say what its passes are` is what found
  it, which is exactly the case that was written to.

---

## Not started

### Stage 3 — the executor (D00002) · step 1 done, step 2 in progress

**The gate is the golden image, and it is not enough on its own.**
`just studio-smoke` must still produce `51931c790b836cb4d37275a276e52890`, and
it is worth re-running after every step rather than at the end. But it only
covers the passes the captured frame actually shows — `surface` is not one of
them, which was found by mutation and is written up under step 2 along with the
counter probe that does cover it.

- [x] **Step 1 — the seam.** `render::PassTable` maps a node's `Kind` to a
      handler; `render::GraphRunner` is the `graph::NodeRunner` that looks one up
      and calls it. No SDL, no device state, so it is testable headlessly —
      `engine.render.graphrunner`, eight cases.

      The one that matters is **the equivalence the rest of the stage rests
      on**: running `StandardGraph` through `Execute` submits
      `shadow, surface@0, opaque@0, transparent@0, overlay, interface` — the
      exact sequence `Renderer::Render` submits today. Four views of one world
      share the shadow pass; two worlds get two. If that holds headlessly, the
      only remaining risk in the swap is the device code *inside* each handler,
      which the swap does not touch and which the golden image covers.

      A kind with no handler **refuses the frame and names itself** rather than
      skipping. A skipped pass renders dark and the scene gets the blame.

- [x] **Step 2 — the extraction.** Each of the six pass blocks inside
      `Renderer::Render`'s per-view loop becomes a handler registered into a
      `PassTable` on `RenderState`. The bodies move unchanged; only what calls
      them changes. Golden image after each pass is moved, not after all six.

      - [x] **2a — `ViewPass`.** The per-view values the passes share are a
            struct in `Renderer.cpp`'s anonymous namespace, held as
            `Impl::CurrentView` and cleared at the top of each view. On the
            state rather than in the loop for `ActiveSlot`'s reason: a handler
            is looked up by kind and called with a `graph::RunContext`, so the
            only thing it already has is the renderer. Hash-identical.

      - [x] **2b — `FramePass`, and `shadow` moved.** The frame's three are an
            `Impl::FramePass` of pointers at `Render`'s locals, set and nulled
            by an `Impl::FrameScope` guard — a guard and not two assignments
            because the failure it prevents is silent: a handler run between
            frames would read a submitted command buffer and a `FrameResult`
            that has already been returned by value.

            The body is `Impl::SubmitShadow`, registered under the `shadow`
            kind by `Impl::RegisterPasses` after the pipelines exist. The call
            site is `State->SubmitPass(Pass::Shadow)` — the same lookup by kind
            the runner does, with the *order* still written out in `Render`.
            `SubmitPass` is the stand-in `Execute` deletes in step 3.

            Hash-identical, and **the gate was mutation-verified**: making
            `SubmitShadow` return before submitting gives
            `35c19203665891a2a1f9dbda446fc57e`, so the golden image does check
            this pass rather than merely tolerating it.

      - [x] **2c — `surface` moved**, and `ViewPass` grew the half it needed:
            the accepted surface cameras, the shared signature, the refresh
            count, the light uniforms, and `scene::ScenePlan` **whole** in place
            of the three numbers the shadow step had copied off the front of it.
            `bank` did not move — the handler derives it from `ActiveSlot`,
            which is already the answer to "which panel".

      - [x] **2d — `opaque` moved**, and with it the blended stage, the
            particles and the ribbons, because **they are one
            `SDL_BeginGPURenderPass`**. `ViewPass` grew the camera's half: the
            camera and its frame, the scene rectangle, `Offscreen`, the six run
            counts, `CameraRuns`, and the effects' spans. `FramePass` grew
            `Swapchain`, which is the frame's and not a view's.

            `transparent` was deliberately left unregistered at this point, so
            `PassTable::Missing` named it rather than an entry claiming to draw
            what `SubmitOpaque` already drew. 2f is what added it.

      - [x] **2e — `overlay` and `interface` moved.** The frame's last two, and
            the only ones that are `NodeScope::Frame`. `FramePass` grew the
            three things they share: a **pointer** to `Render`'s
            `windowTarget`, because the load op flips from clear to load as the
            frame goes on and every writer must see the same one;
            `HaveOverlay`; and `Interface`, which is the hook or null.

            **`Interface` is filled after the view loop, not by `FrameScope`.**
            Dear ImGui copies its buffers through a copy pass and SDL refuses to
            open one while a render pass is in flight, so the hook cannot be
            asked to prepare until every view has finished with the command
            buffer. That is `FrameOverlayHook`'s whole contract.

      - [x] **2f — `transparent` split out.** Two handlers, one render pass:
            `FramePass::Scene` holds the open `SDL_GPURenderPass`,
            `SubmitOpaque` opens it and `SubmitTransparent` ends it. All six
            kinds are registered and `PassTable::Missing` is empty for the
            standard frame.

            **Not two render passes.** The blended draws are depth-tested
            against what the opaque draws wrote and that depth is
            `STOREOP_DONT_CARE`; a second pass would have to store and reload
            it, paying a full depth round trip for a split the frame does not
            need. `PassRecorder` had already settled this — it enters
            `Pass::Transparent` from inside the opaque render pass, and the
            comment there is the answer: *what the list describes is what is
            drawn and in what order, not how many times a target is bound.*

            **Done as two verified steps**, for 2a's reason. First
            `drawScreenMirrors` became `Impl::DrawScreenMirrors` — hash- and
            probe-identical on its own — and only then was the function cut in
            two. Promoting it was most of the work: both stages call it, and it
            re-pushes `ViewPass::Frame` on the way out so the next draw does not
            inherit a mirror's projection.

            Three things fell out that were not in the plan:

            - **`screenSurface` was never shared.** It looked like state the
              plain draws and the mirror draws passed between them; it is reset
              to null before every plain draw, and the plain draws never sample
              a surface. It is now local to `DrawScreenMirrors` and the plain
              draws pass `nullptr` outright.
            - **`Impl::BindingsForWorld`** is the one description of the sampler
              fallback rule, which three stages now take. That was the flagged
              risk — a second copy that forgot `FallbackTexture` is the null
              bind that made a scene of nothing but transparent geometry draw
              with no samplers at all.
            - **`Impl::EndScenePass`** closes the pass on the paths that abandon
              a frame. Submitting a command buffer with a render pass in flight
              is a validation failure, and those paths are exactly the ones
              nobody exercises.

      ### The golden image does not gate every pass

      **Found by mutation, and it changes how the rest of this stage is
      checked.** Making `SubmitSurface` return before submitting leaves
      `51931c790b836cb4d37275a276e52890` **unchanged** — the standard capture
      does not show a pane that samples a surface texture. `shadow` mutates the
      hash; `surface` does not, and no deterministic capture does: the Mirrors
      world only becomes the capture target after several hundred frames, by
      which point the capture varies run to run for `studio-meshes`' reason.

      So the golden image is necessary and **not sufficient**. What was used
      instead for every step of stage 3, and what step 3 itself should use:

      **Probe the frame's own counters against the pre-change build.** A
      temporary `ENGINE_ERROR` at the end of `Render` printing `DrawCalls`,
      `Triangles`, `SurfacePasses`, `SurfaceInstances`, `Passes`, `Culled`,
      `Uploads` and `UploadedBytes`, run for 40 frames with no capture, is
      **deterministic** — two runs are byte-identical. Build it on `HEAD`, keep
      the log, `git stash pop`, build it again, `diff`. For 2a–2c that came out
      identical over 27 frames including `surfacepasses=4`, which is the check
      the hash could not make.

      **Compare the *distinct* lines, not the transcript.** This originally
      diffed all 27 `PROBE` lines byte for byte, which held for most of
      stage 3 and then stopped holding: the mesh grid's content lands on a
      different frame when the machine is busy, so one frame's `tris` and
      `culled` wander by one instance. `sort -u` gives five stable lines and
      still catches a real change — a pass drawing the wrong range produces
      a count that appears nowhere in the other run.

      It is worth keeping this as the routine for anything in `render` that
      claims to be behaviour-neutral: it costs two builds and it covers the
      passes the one deterministic capture cannot see.

      **Three runners, because one does not reach every pass.** The headless
      capture never draws the window, so `overlay` and `interface` are invisible
      to it — and `overlay` needs a debug panel that the studio does not open.
      The full set, and what each is for:

      | run | covers |
      |---|---|
      | `studio --headless --frames 40 --run play` | `shadow`, `surface`, `opaque`, `transparent`. **Deterministic** — diff the 27 `PROBE` lines directly |
      | `studio --frames 30 --run play --stats` | `interface`. Counts drift with timing, so compare the **set of `ran=` values** — `{36, 37, 47}` |
      | `client --stats` (20s, `timeout`) | `overlay`. `ran=21` is `shadow \| opaque \| overlay`; the studio uses ImGui for its panels and never sets bit 4 |

      `ran` is `FrameResult::Passes`, so a missing pass shows up as a missing
      bit whether or not it changed a pixel. Redirect to a file and grep the
      file — `timeout … | grep` loses the output when the timeout fires.

      **And check that the build actually built.** 2d passed the golden image
      while `SubmitOpaque` was written but never registered — `cmake --build`
      had printed nothing and rebuilt nothing, so the hash was taken from a
      stale binary. The probe caught it in one run: no `PROBE` lines at all, and
      `no handler registered for render pass 'opaque'` forty times over. Grep
      the build output for `Building CXX`, not only for `error:`; a hash from a
      binary that does not contain the change is worse than no hash, because it
      looks like evidence.

      **The closure set, corrected by moving it.** The eleven names below were
      measured by reading; four of them are the *frame's* and one is not
      referenced at all, so `ViewPass` has **seven** fields and not eleven:

      | name | where it belongs |
      |---|---|
      | `haveInstances`, `sceneCount`, `sceneReflected`, `reflectedCasters`, `surfaceCasters`, `lightViewProjection`, `haveShadow` | the view — these are `ViewPass` |
      | `command`, `passes`, `result` | the **frame**. Every view submits into one command buffer and adds to one `FrameResult`; a per-view struct holding them would reset the frame's totals on the second viewport |
      | `State` | the renderer, and already reachable |
      | `target` | not referenced. The shadow block's target is `shadowTarget`, its own local |

      So the frame's three still want somewhere to live before a handler can
      read them — a `FramePass` beside `ViewPass`, pointed at `Render`'s locals
      for exactly the length of one call. That is step 2b's first move, not a
      second `ViewPass` field.

      **`just studio-meshes` is not a hash gate.** The second capture in
      `studio-smoke` came out different after a change that could not have moved
      a pixel; three runs of the *unchanged* binary then gave three different
      hashes. It renders 700 frames while content arrives, so it answers "did
      anything draw", not "did this draw the same". Only
      `51931c790b836cb4d37275a276e52890` on the first capture is the gate.

      **Order to move them in**: `shadow` (smallest, no colour target, cleanest
      boundary), then `surface`, `opaque`, `transparent`, then `overlay` and
      `interface` — the last two sit *outside* the per-view loop and so need
      nothing from the per-view struct.

      ### Next: 2d — `opaque`, and the thing in the way

      **`opaque` and `transparent` are one `SDL_BeginGPURenderPass` today.** The
      graph has two nodes; the renderer has one render pass with the blended
      draws in its tail, sharing the colour and depth attachments, the viewport,
      the scissor and the light push. Two handlers that each began their own
      render pass would have to decide load and store ops for the second — and a
      `LOADOP_CLEAR` there wipes the opaque geometry while a `LOAD` is a
      different frame from the one this hash was taken of. **That decision is
      the step, not the extraction**, and it wants making before any code moves.

      Three ways out, in the order they look worth trying:

      1. **One handler for both**, registered under both kinds, with the second
         call a no-op. Cheapest, keeps the frame identical, and is honest only
         while the two are adjacent — the moment somebody reorders them in the
         editor it is a lie, which is the whole thing stage 3 is for.
      2. **Two handlers, one render pass**, with the pass object living on
         `FramePass` and opened by whichever runs first. Keeps the frame
         identical and makes the adjacency explicit rather than implicit, at the
         cost of a render pass that outlives the handler that opened it.
      3. **Two render passes**, `LOADOP_LOAD` on the second. Honest, matches
         what the graph says, and is the one that has to be checked against a
         capture rather than reasoned about — it changes bandwidth and, on a
         tiler, possibly the image.

      **What `opaque` closes over — measured the same way as `shadow` was.**
      Thirty-five names, of which `command`, `passes`, `result` and `swapchain`
      are the frame's and the rest are the view's:

      `bank`, `blended`, `camera`, `cameraFrame`, `cameraRuns`, `captureHeight`,
      `captureReady`, `captureSlot`, `captureWidth`, `empty`, `instanceCount`,
      `offscreen`, `opaqueCount`, `particleCount`, `particles`, `plainOpaque`,
      `plainTransparent`, `ribbonCount`, `ribbonRuns`, `sceneHeight`,
      `sceneWidth`, `shadow`, `shadowSampler`, `surfaceInCamera`,
      `surfaceSampler`, `targetSlot`, `transparentCount`, `transparentSurfaces`,
      `windowTarget`

      That is four times the shadow block's and it is why this is the step with
      a decision in it rather than a bigger copy. Several collapse: `targetSlot`
      is `ActiveSlot`, `bank` is `SurfacesAt(ActiveSlot)`, and the `capture*`
      four belong to the capture rather than to the pass.

      **The blocks, located** (line numbers drift; the `--- <name> pass ---`
      banners are the durable markers):

      | pass | banner |
      |---|---|
      | shadow | `// --- shadow pass ---` |
      | surface | `// --- surface pass ---` |
      | opaque | `// --- opaque pass ---` |
      | transparent | inside the opaque block's tail |
      | overlay | `// --- overlay pass ---`, after the view loop |
      | interface | `// --- interface pass ---`, after the view loop |

      **What `shadow` closes over — measured, not guessed.** This is the whole
      difficulty of the step, and the first block is the small one:

      `command`, `passes`, `State`, `result`, `target`, `haveInstances`,
      `haveShadow`, `sceneCount`, `sceneReflected`, `reflectedCasters`,
      `surfaceCasters`

      Eleven names, of which `State` and `command` are the frame's and the rest
      are per-view. So **the enabling change is a `ViewPass` struct** holding the
      per-view values, built once at the top of the view loop and reachable from
      a handler. That change alone should be behaviour-neutral and
      hash-identical, and it is the one to verify hardest — it is also the one
      that makes the remaining five blocks cheap rather than each being its own
      surgery.

      Later blocks close over strictly more: `plan`, `frameUniforms`,
      `lightUniforms`, `uploadCount`, `windowTarget`, `instanceCount`. Add to
      `ViewPass` as each pass needs them rather than trying to guess the union up
      front — a struct grown one pass at a time is reviewable; one written from a
      grep is not.

- [x] **Step 3 — the swap and the deletion.** `Render` calls `Execute`; the walk
      is gone. `Renderer::Render` went from **1333 lines to 389**.

      **The enabling change was `Impl::PrepareView`**, not the call to `Execute`.
      Culling, ordering, the surface partition and the one copy pass that feeds
      them are what a pass *reads* — they are not passes, and the graph has no
      word for them. Splitting them out of the view loop is what left something
      `Execute` could drive.

      **Preparation is lazy, and has to be.** There is one instance buffer, so
      view N's ranges overwrite view N-1's — a view has to be prepared
      immediately before its own passes. `Impl::FrameRunner` does it: every node
      carries the view it is for, and the first node of a view is where that
      view's preparation belongs. A `World` node belongs to its world's *first*
      view, which is both a representative — the light is fitted to the whole
      scene bound, not a camera frustum, so every view of a world would fit the
      same map — and a necessity, since the shadow pass draws from a buffer
      nothing has uploaded to until some view is prepared.

      **The interface prepares on the first `Frame` node**, which is the first
      moment no render pass is in flight. That was a comment in `Render` about
      `FrameOverlayHook`'s contract; it is now a place in the walk.

      What went, and what stayed:

      - **`Pass` gone.** It was the hand-written half — six enumerators in the
        same order as six node names, with a test keeping them in step. A test
        that compares two descriptions can say they disagree and not which is
        right, and only runs after somebody has already changed one.
      - **`PassRecorder`'s ordering guard gone.** `Execute` walks the compiled
        order, so the order is derived rather than asserted. A second check of it
        would be a second description.
      - **`SubmitPass` gone** — it was the stand-in for `Execute`.
      - **`PassOrder()` stayed**, because it never was the duplicate: it reads the
        names out of `StandardGraph`. It is now the index space for
        `FrameResult::Passes`, and `Ran` takes a `core::Name` rather than an
        enumerator.
      - **`BuildFrameGraph` arrived**, which compiles the standard frame at
        start-up and asks `PassTable::Missing` whether this renderer can draw
        every node in it. That is the one place that question is worth asking,
        and asking it at start-up rather than mid-frame is the difference between
        a diagnostic and half a frame.

      **The behaviour change that was predicted did not happen, and that is
      correct.** Both callers — `studio::Editor` and `client::Client` — pass
      exactly one view per `Render`, and `View::World` is never set. So the
      shadow pass still runs once per `Render` call. The `NodeScope::World`
      grouping is live and will share a shadow map the moment anything passes
      several views, which is what `engine.render.graphrunner` already asserts
      headlessly.

      **Unblocked.** All six kinds are registered, so `PassTable::Missing` is
      empty for the standard frame and `Execute` has something to call for every
      node it walks.

      **One ordering constraint the swap has to respect**, and it is the only
      thing 2f left behind: `opaque` leaves a render pass open and `transparent`
      ends it, so nothing may run between them. `Execute` walks the compiled
      order, and `Compile` puts them adjacent because the blended stage reads
      what the opaque stage wrote — but that is a *consequence* of the
      dependencies rather than something the graph states. If a future pipeline
      ever schedules a node between the two, `EndScenePass` is what stops it
      being a crash, and the honest fix would be for the node pair to say they
      are merged. That is Unity's merge bar, and §7 of the design is where it
      belongs.

      **The one behaviour change to expect, and it is intended.** The shadow
      node is `NodeScope::World`, so `Execute` runs it **once per world**; today
      it runs once per *view*. Four viewports of one world render the shadow map
      four times now and once afterwards. The matrix is fitted to the whole
      scene bound rather than the eye's frustum, so all four are the same map —
      which is exactly the claim `graph::StandardGraph` makes and
      `engine.render.graphrunner` already asserts. Expect the golden image to
      hold and the *cost* to drop; if the image moves, the scope is wrong rather
      than the swap.

      `SubmitPass` and the six call sites in `Render` are what `Execute`
      replaces, so the swap is small — the six steps above are what made it so.

### Stage 7's other half — per-pass GPU timestamps (D00046)

**Not blocked on stage 3 any more, and not buildable either: `SDL_GPU` has no
timestamp query API.** The entry said "SDL_GPU exposes timestamp queries"; it
does not. `SDL_gpu.h` at the vendored 3.2.31 has fences —
`SDL_SubmitGPUCommandBufferAndAcquireFence`, `SDL_QueryGPUFence` — and those are
whole-command-buffer granularity, which is one number for the frame. There is no
query pool, no timestamp write, nothing per pass. Checked by reading the header
rather than by remembering.

So this is blocked on SDL rather than on us, which is a different kind of
blocked: no amount of work here moves it.

- [x] **Debug groups instead**, which is the half that *is* available.
      `FrameRunner::Run` pushes `SDL_PushGPUDebugGroup` named for the node and
      pops it when the render pass closes — one group spans `opaque` and
      `transparent` because they share a pass and SDL asks that a group pushed
      inside a pass be popped inside it, so the second names itself with
      `SDL_InsertGPUDebugLabel`. RenderDoc, Nsight and Xcode now attribute every
      draw to a node. That is the *readable capture* half of §7; the *numbers*
      half needs an API that does not exist.

- [ ] `ProfilePass::Elapsed` stays zero and the panel keeps saying **not
      measured**, which is the honest state. **Do not fill it with CPU time.** A
      submit-side number in a field labelled as the pass's cost is worse than a
      blank: somebody reads "0.4 ms" for the shadow pass, believes the GPU said
      it, and spends an afternoon optimising the wrong thing.

- [ ] What would actually unblock it, in the order worth trying: an SDL release
      that adds timestamp queries; or a backend-specific path behind
      `Renderer::Backend()`, which already hands out the `SDL_GPUDevice` — Vulkan
      has `vkCmdWriteTimestamp` and a query pool, and D3D12 has
      `ID3D12GraphicsCommandList::EndQuery`. That second one is a real option and
      a real cost: it is per-backend code in a module whose whole point is not
      being per-backend.

### Stage 9 — the entity flow: what a pass draws, as a wire

**The half of a frame that was never in the graph.** Stages 1–8 made the frame
authorable in every respect but one: a pipeline could add a pass, reorder passes
and retarget them, and could not say **which geometry any of them took**. Culling,
tag filtering and ordering were a fixed sequence in the middle of
`Renderer::Render` and every pass read its result. A pipeline that can only
reorder the frame somebody else wrote is not an editable pipeline.

So a list of instances is a resource and the operations on it are nodes:

```
entities ─▶ cull-frustum ─▶ filter-tag ─▶ order-draw ─▶ opaque ─▶ output
                 │
            filter-tag ─▶ shadow
```

- [x] **`ResourceKind::Entities`** — indices into the view's draw list.
      *Indices, not pointers*: they stay valid as long as the caller's span
      does, cost four bytes, and two lists of the same geometry share nothing
      that can go stale. "Pointers" is the right idea and the wrong
      representation.

- [x] **`graph::EntityFlow`** — the store the lists travel in, plus
      `AllEntities`, `FilterByFrustum`, `FilterByTag`, `FilterByDistance` and
      `OrderEntities`. All arithmetic over spans, all in L9 with no device, all
      exercised by `engine.graph.entityflow`.

      **`scene::OrderSubset` rather than a second sort.** `OrderForDrawing` had a
      reverse-then-stable-sort that a test caught once — "equal distances keep
      world order" is only true because of it. Generalising it to a subset and
      making the old entry point call it is what keeps that one description.

- [x] **Seven node kinds**: `entities`, `cull-frustum`, `cull-distance`,
      `filter-tag`, `order-draw`, and `output` — the terminal the pipeline hands
      its image back through. The geometry kinds gained an **optional** entity
      input each.

- [x] **`Impl::SubmitFlow`** — one handler for the five, because they differ only
      in the predicate. Each reads the first entity resource it is given, writes
      the first it produces, and never looks at the world.

**Three faults the building turned up, each caught by an existing check:**

- **Catalogue inputs are positional, and prepending shifts every wire.**
  `PipelineDiagnostics` maps `Node::Reads[n]` onto `NodeKindSpec::Inputs[n]`, so
  putting the new entity slot at the *front* of `opaque` silently re-pointed
  every existing wire at the wrong slot — which surfaced as every colour target
  in the frame reporting `format-overspend`. **Append to `Inputs`, never
  prepend**, until slots are matched by name.

- **An entity list has no pixel format.** Its slot declares one only because
  every slot does; the format checks read that as "eight bits a pixel" and
  reported every HDR target as overspent. `PipelineDiagnostics` now skips
  `Entities` resources rather than being given a sentinel format — a format
  meaning "not an image" would have to be understood everywhere a format is.

- **`Source` meant "reads nothing" and had to mean "reads no image".** `shadow`
  and `depth-prepass` are sources — they need no picture from anywhere and can
  start a chain — and they now take an optional entity list. The rule is
  narrowed rather than holed: a source's inputs must all be `Entities` **and all
  be optional**, because a source with a required input cannot start a chain
  whatever it carries.

- [x] **The last link — a filter now changes the frame.** `PrepareView` builds
      the camera range from whatever entity list the pipeline wired into
      `opaque`, and falls back to its own cull and sort when there is none. So
      the standard frame is byte-identical and a pipeline with filters in it
      draws what the filters left.

      **The entity nodes run at prepare time, and that is forced rather than
      chosen.** The camera range is uploaded once for the whole view, before any
      pass; nodes that decide its contents therefore cannot wait for
      `graph::Execute` to reach them. `RunEntityFlow` walks
      `CompiledGraph::PerView` and runs exactly the nodes whose *output* is an
      entity resource — still the graph's nodes, still in the order `Compile`
      produced — and `SubmitFlow` finds the work done when `Execute` arrives.

      **`DrawOrder` is the identity on the wired path.** The list already came
      out of `order-draw`, so re-sorting it would be doing the sort twice; the
      opaque count is taken by walking to the first blended instance, which is
      only correct *because* `order-draw` puts the opaque head first. A pipeline
      that omits `order-draw` gets its blended geometry in world order — the
      author's decision, and visibly so.

      `upload-instances` is in the catalogue as the honest name for that join,
      and is not yet a node the renderer runs: the upload is still one operation
      covering the scene range and the camera range together, and splitting it
      is what would let two colour passes take two different lists.

- [x] **The camera is a value, not ambient state.** `ResourceKind::Camera` and
      `graph::Viewpoint`; `camera` and `light-camera` source nodes;
      `cull-frustum`, `cull-distance` and `order-draw` take an **optional**
      camera input.

      **This fixed a limitation the entity flow introduced.** `SubmitFlow`
      hardcoded `CurrentView.CameraFrame`, so every filter culled against the
      eye and a pipeline could not say *cull against the light* or *order for
      this mirror*. §4.7 of the design had asked for `camera` as a wire from the
      start — "parameters as wires rather than as hidden state" — and this is
      that.

      **`light-camera` is what makes "do not cull the shadow pass" sayable as
      something better.** The rule was *never frustum cull the casters*, because
      one off screen still shadows in. With the light as a viewpoint the honest
      version is *cull against the light's box*, which drops what casts into
      nothing instead of keeping everything.

      A `Viewpoint` carries either a lens or a **fitted** projection, because
      `FitDirectionalLight` produces a box from the scene bound and there is no
      field of view to describe it with.

- [x] **A pipeline per camera, and a `world` source node.**
      `Renderer::SetPipeline(name, graph)` holds a table; `View::Pipeline` names
      which one that camera runs; unset runs the default.

      **The authoring side already existed and was unreachable.**
      `graph::PipelineSet` is a *named set* of pipelines registered as a world's
      resource, and its own comment says a world does not have *a* pipeline any
      more than it has *a* script — a main chain, a cheap one for a reflection,
      a debug one somebody switches to. Nothing could select from it, because a
      view had no way to name one.

      **`Execute` cannot express this**, so it was split into the two halves it
      was made of: `ExecuteView(compiled, runner, view, world, shared)` and
      `ExecuteFinal(compiled, runner)`. `Execute` is now those two composed —
      one description, and the split is behaviour-neutral.

      Two rules the frame loop keeps:

      - **The shared block runs once per (world, pipeline).** Two cameras of one
        world running one pipeline share its shadow map, which is what
        `NodeScope::World` means. Two cameras of one world running *different*
        pipelines do not — the second pipeline's shared work is not the first's.
      - **The frame's own block always comes from the default pipeline.** A
        window has one overlay however many cameras drew into it.

      **A gotcha worth knowing before authoring one:** `Compile` puts a
      `Frame`-scoped node that appears *before* any per-view node into `Shared`,
      not `Final` — with nothing per view, "before every view" and "after every
      one" are the same block. So a frame pipeline of nothing but overlays runs
      from `ExecuteView` and `ExecuteFinal` does nothing. Pinned by a case.

- [x] **Node parameters** — `Node::Parameters`, a `set "key" "value"` word in
      the document format, and `Number`/`Integer` readers.

      **The difference between a kind and a node.** Two `filter-tag` nodes are
      the same kind and filter different tags; two `raster` nodes are the same
      kind and run different shaders. Without this a pipeline could say *which*
      passes run and never *how*.

      **Text, deliberately.** A parameter is authored text that has to survive a
      save file, a diff, and somebody typing it. Parsing it into a number at the
      graph layer would put the parse in the wrong place and give two answers
      for what an unset one means; instead whoever reads a parameter decides
      what its absence means — which is how `filter-tag` reads no mask as *keep
      everything*.

      **Unset and set-to-nothing are different**, and only the first takes a
      default. `Parameter` returns null for one and an empty string for the
      other.

      **Unreadable falls back rather than refusing.** A half-typed number in an
      editor is a state somebody is passing through, not a pipeline to reject —
      `strtof`/`strtoul`, not `stof`. `Integer` reads base zero, so a tag mask
      can be written `0x0f`.

      This closed the two gaps flagged above: `cull-distance` reads `radius` and
      `filter-tag` reads `mask`, both from the node.

- [x] **Graph-allocated targets.** `Impl::EnsureGraphTarget` makes a texture
      from a `ResourceDesc` — its format, and its absolute size or the view's
      rectangle over `Divisor`. `TextureFor` answers for the renderer's own six
      names first and falls through to this for anything else a pipeline
      invented.

      **The first thing the renderer allocates from a description rather than
      from its own list.** Every other texture exists because `Renderer` was
      written to have one; these exist because somebody's pipeline said so. That
      is what makes a half-resolution pass a number in a document rather than a
      second code path.

      **External resources are refused.** `window` and `colour` name things
      outside the graph, and allocating for one would make a second window
      nobody presents.

      `NamedTexture` gained a `Format`, because SDL will not hand a texture's
      format back and a pipeline is built against the format of what it renders
      into — so whoever produced the texture is the only one who can say.

- [x] **A shader-keyed pipeline cache.** `Impl::PipelineForShader`, keyed on the
      shader name *and* the target format. **`PassTable` keys on the kind and two
      `raster` nodes are one kind and two pipelines** — that mismatch is the
      whole reason this exists rather than the table doing it.

      Cached even when it fails, so a shader that does not exist is looked for
      once rather than once per frame per view. The log line is the diagnostic;
      a thousand of them is not.

- [x] **The `raster` kind and its handler.** `Impl::SubmitRaster` reads the
      node's `shader` parameter, finds the colour it writes, binds what it reads
      as samplers in slot order, and draws three vertices.

      **The vertex stage is `overlay.vert` unchanged**, which already builds a
      fullscreen triangle from three indices and hands out a UV. Every custom
      raster pass shares it, so where a fullscreen pass puts its vertices is
      described once.

      **Half-configured draws nothing rather than refusing.** A pipeline being
      edited has nodes with no shader, no target or a shader that will not
      build, constantly. Taking the window down for one is not a diagnostic.

- [x] **The renderer no longer reads shaders.** `Renderer::AddShader(name,
      spirv)` — the same shape `AddMesh` and `AddTexture` already have, so a
      shader from the content store arrives by the door a texture does. Supplied
      wins over staged, because a caller that went to the trouble of delivering
      one means it; the staged directory stays as the developer path and the
      fallback.

      **SPIR-V, not source.** Compiling is `bake`'s business and happens once,
      off the machine that is drawing — a renderer holding a shader compiler for
      content it did not write is how a frame ends up paying for a parse.

      **Replacing one drops every pipeline built from it.** A hot reload, or a
      newer version arriving from the store, that left its cached pipeline in
      place would look exactly like the delivery having failed — and that is the
      failure somebody would spend the afternoon on.

      Verified on a device with a shader that **exists in no file**: 1292 bytes
      handed over under `delivered.frag`, a pipeline built from it, and the pass
      drawing at 1600x879.

- [x] **The store side: routing and delivery.** `AssetKind::Shader`, the
      extensions that reach it, and the studio handing a delivered module to
      `Renderer::AddShader`.

      **Its own kind rather than `Script` or `Data`.** `Script` is source a VM
      may run in a sandbox; `Data` is bytes handed over whole. A shader is
      neither — compiled ahead of time, handed to a GPU rather than an
      interpreter, and whether it is *safe* is a question about a driver rather
      than about a sandbox. Routing it as either puts it through the wrong
      subsystem's door.

      **Source and compiled both route here**, because what somebody publishes
      is what they wrote — `spv`, `frag`, `vert`, `comp`, `glsl`.

      **And the sources are in `IsRuntimeReadable`'s list**, which is the row
      that would have been forgotten. That function's own comment names the
      failure: a format added to the extension table without a row there "would
      be offered as loadable and would not load". A renderer holds no shader
      compiler, so GLSL reaching a runtime has to be caught at the manifest
      rather than at the draw. Both mutations checked red — routing `frag` as
      `Script`, and dropping the sources from that list.

      `ContentShaders` counts what arrived, beside the mesh and texture counts,
      so somebody can tell "the pipeline is wrong" from "the content has not
      landed".

- [x] **Compiling at runtime, which this engine already decided to do and
      grug had not noticed.** `Renderer::CompileShader(name, glsl, stage,
      error)`.

      **A correction to what is written two bullets up.** `AddShader`'s first
      draft said compiling is `bake`'s business and a renderer holding a
      compiler is how a frame pays for a parse. `mono.build/MonoVendor.cmake`
      had already settled the opposite, in writing, and for a good reason:

      > *"a `ShaderScript` whose revision changed, a swapped antialias pass, a
      > shader permutation — none of them exist at build time, so none can be
      > compiled ahead of it."*

      `libshaderc` is linked into the client **on purpose**, and
      `render/src/ShaderCompiler.cpp` already existed. The two paths are not in
      competition: a module published to the store is baked once and delivered
      as SPIR-V; a shader somebody is editing has no baked form yet and will
      have a different one a keystroke later. `AddShader` is the first,
      `CompileShader` is the second, and both end in the same table.

      **The error is returned, not logged.** Whoever typed the shader is the one
      who can fix it, and a compiler message in a log they are not reading is a
      message nobody gets.

      Verified on a device: GLSL passed as a C string compiled, registered and
      built a pipeline with no file anywhere; deliberately broken GLSL came back
      with a non-empty message and no pipeline.

- [x] **Live shader editing, and the document carries the shader.** A `source`
      parameter holds GLSL; the renderer compiles it when the text changes and a
      box in the Render Pipeline panel is where somebody types it.

      **The save file is the whole pipeline, not a reference to files beside
      it.** `AppendQuoted` already escaped newlines — it was written for names
      and turns out to carry a shader — so a `set "source" "..."` line survives
      a round trip byte for byte, tabs and all. Pinned by a case, because a
      shader that came back with its whitespace rearranged would compile and
      diff forever.

      **Source wins over `shader`.** A name refers to something delivered or
      staged; source is what somebody is typing. A node carrying both means the
      one being edited.

      **Keyed on the node and prefixed `~`**, so two nodes editing two shaders
      do not collide and a live shader can never be confused with a delivered
      one of the same name.

      **Recompiled only when the text changes.** A compile per frame per view
      would turn a text box into a stall, which is the whole thing live editing
      is supposed not to be.

      **The error goes under the box.** Kept on the node rather than logged
      once: a compiler message in a log somebody is not reading, while they are
      staring at the shader that caused it, is a message nobody gets.

      Verified on a device: GLSL carried in a `RenderGraph` node compiled and
      drew as `~live` at 1600x879, and a deliberately broken edit came back with
      a message and no pipeline.

- [ ] **The bake step, and it cannot live in `bake`.** A `.frag` published today
      routes to `AssetKind::Shader`, is correctly refused as not-yet-readable,
      and nothing turns it into a `.spv`.

      **`bake` is `TIER shared` and links no shaderc** — `MonoVendor.cmake` gates
      the whole vendor on `MONO_BUILD_CLIENT`, and a server configure gets none
      of it. So does `Tool::assetc`, which wraps `bake` and is `shared` too. The
      whole baking chain is below the tier the compiler lives at.

      **`contentimport` is the one place that can link both**, and its own
      CMakeLists already explains why that pattern exists — for `cdn`:

      > *"The baking cannot move into `Mono::cdn` — its short link row is the
      > whole point of that member and a baker is exactly the interpretation an
      > origin must not do — so it lives in the programs that can link both."*

      A shader baker is the same shape one layer along. It is a bare
      `add_executable` with no tier, so `Vendor::shaderc` can go on its link row.

      **What that costs, and it should be decided rather than discovered:**
      `contentimport` would stop building in a server-only configure, because
      shaderc is not configured there at all. Today it builds anywhere. Either
      that is acceptable — it is a developer's tool and a server has no shaders
      to publish — or the shader baking wants its own small client-tier program
      beside it, and `contentimport` calls out to that.

      Worth knowing before somebody adds `Vendor::shaderc` to `bake` and finds
      out at the architecture check instead of at the design.

      **The interesting decision is includes.** A shader that `#include`s
      another is a dependency between two assets, and the manifest has no word
      for one. Either the baker resolves them and publishes a flat module —
      simple, and a shared header edit rebuilds every shader — or the store
      learns about dependencies, which is a bigger thing than shaders.

      **Split deliberately.** The seam above is the half that had to be in
      `render` and is now done and proven; the half that is left is content
      plumbing that can be built and tested on its own, without a GPU.

- [x] **Where the shader comes from — staged SPIR-V, and the path is proven.**
      `mono.engine/render/shaders/tint.frag` is a real custom pass; the build
      stages it to `tint.frag.spv` like every other shader, and the renderer
      loads it by the name a node gives.

      **Checked on a real device, not only in a table.** An authored pipeline
      with a `raster` node was set as the default and run headlessly: the pass
      executed into targets the *graph* allocated — 960x519 and 448x270, sized
      from the view — with a pipeline built from `tint.frag` and no validation
      errors. Probe removed afterwards.

      A shader from the content store is the honest end state and is a content
      question as much as a graph one: baked, published, fetched and
      version-checked like a mesh. Staged is enough to prove the seam and is
      what ships today.

- [x] **`dispatch` — the compute half, and it is not the raster one with other
      words.** `Impl::SubmitDispatch` and `ComputeForShader`;
      `shaders/desaturate.comp` proves the path.

      **It writes `ResourceKind::Storage`, not a colour attachment**, which is
      the distinction that kind was made for and the one §1.5 fault 10 is about
      — a copy done as a full-screen triangle is invisible when everything is
      "a pass". `EnsureGraphTarget` now picks the usage flags from the resource's
      kind: `COMPUTE_STORAGE_WRITE` for storage, `DEPTH_STENCIL_TARGET` for
      depth, `COLOR_TARGET` otherwise, and samplable in every case because the
      point of writing one is that something reads it.

      **No format in the pipeline key**, unlike `raster`. A compute pass writes
      through storage rather than into an attachment, so the pipeline does not
      depend on what it writes into.

      **The group size is declared twice by necessity** — `local_size_x` in the
      shader and `threadcount_x` in the pipeline — and SDL is explicit that it
      does not check they agree. So the node says it (`group-x`, `group-y`,
      default 8) and the renderer passes it on rather than assuming a number.

      **Whole groups, rounded up, and the shader checks its own bounds.** A
      target whose size is not a multiple of the group size has a last row and
      column that run past the edge; refusing to dispatch them would leave a
      strip unwritten instead. Verified on a device at `1088x339` into `136x43`
      groups — a size that exercises exactly that rounding.

- [x] **Barriers: there are none to write, and the real hazard is cycling.**

      This was filed as "derive the barriers RDG derives". Checking the header
      first — `PIPELINE_NODES.md`'s own lesson, and the one the timestamp entry
      was written by — says there is nothing to derive: **`SDL_GPU` synchronises
      itself.** Its contract is cycling, not barriers:

      > *"you don't have to worry about complex state tracking and
      > synchronization as long as cycling is correctly employed"*

      **But the same paragraph names a hazard nothing here was checking:**

      > *"When cycling, all data in the resource is considered to be undefined
      > for subsequent commands until that data is written again. You must take
      > care not to read undefined data."*

      Every target this engine writes is cycled. So a fullscreen pass wired to
      **sample the target it draws into** does not read last frame's image or
      this one's — it reads undefined memory, which on most drivers looks like a
      plausible frame most of the time. That is the worst way for a bug to
      behave, and it became reachable the moment `raster` let somebody wire one.

      Audited first: every `load_op`/`cycle` pair in the renderer is
      `CLEAR` + cycle, or `LOAD` + no cycle. **No existing pass has the bug** —
      the check is for what a pipeline somebody authors can now do.

      `DiagnosticKind::SamplesOwnTarget` is that check, and it is static —
      `engine.graph.pipelinediagnostics`, no GPU.

      **The exemption is the interesting half.** `transparent` reads and writes
      `colour` and is perfectly correct: it blends onto the attachment inside one
      render pass, which the hardware does natively. A check that fired on "reads
      and writes the same resource" would report the standard frame as broken,
      which is how a diagnostic teaches people to ignore it. So it asks the
      *catalogue* which slot is a `Texture` — sampled — and which is an
      attachment, rather than guessing from the resource's kind.

      Three cases, and both mutations red: exempting nothing reports the standard
      frame; never firing misses the blur. A read-modify-write wants two
      resources and a ping-pong, which is what every engine's blur and every
      temporal resolve already does — and the graph can say that.

- [x] **Parameters reachable from the canvas.** `nodeview::EditorNode` carries
      them, `ToDocument` emits the `set` word, `FromDocument` puts them back,
      and the Render Pipeline panel has a field per setting on the selected
      node.

      **Found by asking whether anybody could actually use the feature.**
      `Node::Parameters` existed, the document format had a word for it, and the
      renderer read it — and `ToDocument` had no writer for that word. So
      anything typed on a canvas was dropped at the next save, and `raster` was
      authorable in code only. A feature reachable only from C++ is a feature
      the editor does not have.

      **Sorted by key on the way out**, so a document written twice with the
      same contents is byte-identical whatever order somebody typed them —
      `PipelineSet::Names`' argument about a save file.

      **A `set` before any node is dropped, not applied to the next one.**
      Silently moving somebody's shader onto a different pass is worse than
      losing it: a lost setting is visible and a moved one is a mystery.

      **Empty removes rather than storing an empty string**, because `unset` and
      `set to nothing` are different answers to `Node::Parameter` and only the
      first takes the default that makes an unconfigured node a no-op.

      The offered keys are a convenience and **not a schema** — anything already
      set is shown whether offered or not, so a hand-edited document or a kind
      that grows a parameter later is visible rather than silently carried.

- [x] **Two colour passes, two lists.** `ViewPass::CameraRange` — a list, a base
      into the buffer, and its own three runs. `Ranges` holds one per list a
      colour pass is wired to, and `RangeFor` is how a handler asks for its own.

      A pipeline with no flow nodes gets exactly one range, built from the cull
      and sort the renderer has always done — which is what keeps the standard
      frame byte-identical.

      **The base is added in exactly one place.** A run inside a range is an
      offset within that range; the draw adds `SceneCount + Base`. A run that
      already carried the base would count it twice, which is the arithmetic
      mistake this split makes possible and the reason it is written down.

      **What went with it:** `CameraEntityList` and the warning it carried. That
      warning said two lists do not work — it was true when written and false an
      hour later, and a warning that says a working thing is broken is worse
      than none.

      Verified on a device: `ranges=2`, `opaque list='all' base=0`,
      `transparent list='few' base=33` — two passes, two lists, two slices.

      **And the counter probe had to change.** It compared 27 `PROBE` lines byte
      for byte, and stopped being deterministic partway through this work: the
      mesh grid's content lands on different frames when the machine is busy, so
      one frame's `tris` and `culled` wander. Comparing the **distinct** lines
      (`sort -u`, five of them) is stable across runs and still catches a real
      change — a pass drawing the wrong range produces a count that appears
      nowhere in the other run. Checked three runs each side, before and after,
      identical.

- [ ] **Node parameters.** `cull-distance` has no radius and `filter-tag` no
      mask, because the document format has no word for a node parameter yet.
      Both read their unset value as *keep everything*, so an unconfigured node
      is a no-op rather than a black frame — which is the right default and not
      a substitute for the feature.

### Stage 8 — readbacks (D00047)

**The arithmetic half is built; the device half is not.** Same split as
`PassTable`/`GraphRunner` took, and for the same reason: `render` is the one
module a suite cannot exercise, so everything that can be moved out of it
should be. `engine.render.readback` is eight cases with no GPU in them.

- [x] **Channel histograms — fault 3, and fault 4 with it.**
      `render::Histogram` reduces a downloaded image into four
      `ChannelHistogram`s; `Constant()` is "every pixel the same", `Blank()` is
      "constant and zero", and `ImageHistogram::Uniform()` is the whole-target
      version. That is the check that took a specialist half an hour of reading
      a capture, as arithmetic.

      Three distinctions the cases pin down, each of which was a decision:

      - **Constant is not blank.** A mask that is all ones is doing its job; a
        channel that is all zero is unwritten or wasted. Only the second earns
        a warning triangle.
      - **Empty is not constant.** A target nobody downloaded reports
        `Counted == 0` and is accused of nothing. Reporting "constant" for it
        would put a triangle on a node whose only crime is not being looked at.
      - **Sixteen buckets, not 256.** The question is "blank / two-valued / uses
        its range", and sixteen bars answer all three in a panel a few hundred
        pixels wide. 256 bins are a photograph; this is a diagnosis.

- [x] **The download *policy*** — `render::PendingReadback`. One in flight,
      never stall, and report the age. It takes "is the fence signalled" as an
      answer rather than asking, which is what makes it testable.

      **`Poll` returns true exactly once**, on the edge that made pixels
      readable, because that is when a caller maps the transfer buffer — one
      that kept saying true would re-map and re-reduce the same image every
      frame.

      **The picture is aged from the request, not the fence.** A panel saying
      "one frame old" when the answer is three is worse than saying nothing: it
      is a number that looks measured.

      All four mutations were checked red: swapping the red and blue unpacking
      (3 cases), `/255` instead of `/256` in the bucket (2 cases), ageing from
      the fence (1 case), and dropping the in-flight guard in `Poll` (1 case).

- [x] **The device half.** `Impl::ReadbackSlot` holds a transfer buffer and a
      fence that outlive the frame — the only two things the renderer
      deliberately keeps across one, which is why they are also the only two
      `Shutdown` would otherwise miss. `PollReadback` asks
      `SDL_QueryGPUFence` at the top of a frame and never waits; the submit
      keeps the fence instead of waiting on it.

      **One exception, and it is not a stall.** When `--capture` is also active
      it has already waited, so the download in the same command buffer is
      finished too and is collected there rather than left for next frame. The
      readback did not become a stall by sharing the ride.

- [x] **The `viewer` node.** A handler that turns `RunContext::Reads` into a
      texture and asks for a download of it. It draws nothing and never refuses
      a frame — nothing downstream reads what a viewer produces, so a download
      that could not start is a stale panel and not a hole in the frame.

      **`Impl::TextureFor` is what makes it possible**, and it is the first
      place the graph's resource *names* mean anything: until now `Reads` and
      `Writes` were checked against each other and against nothing else. It
      answers for the standard frame's five plus `overdraw`; anything else gets
      an invalid answer rather than a guess.

      `surface` answers with index zero's readable half, which is a choice
      rather than a fact — there are `scene::MAX_SURFACES` of them and the name
      does not say which. A per-index viewer needs a word the catalogue does not
      have.

- [x] **An overdraw view — fault 9.** `overdraw` is a catalogue kind now, a
      pipeline, a fragment shader and a handler. Three things make it a counter
      rather than a render:

      - an `R8_UNORM` target with **additive** blending, so each fragment adds
        one step of 1/255 and the readback multiplies back up;
      - **depth test and write both off** — the point is to count the fragments
        the depth test would have discarded, and leaving the test on would count
        one per pixel and measure nothing;
      - **no culling**, because a back face still costs a fragment on the way to
        being rejected on some hardware and a count that pretends otherwise
        flatters the scene.

      **The vertex stage is `opaque.vert` unchanged**, so what is counted is
      what that pass would actually shade — same instancing, same clip. A second
      vertex shader would be a second description of where the geometry is, and
      the whole point is to measure the first one.

- [x] **`Renderer::SetPipeline`**, without which all three of the above are dead
      code: `viewer` and `overdraw` are deliberately **not** in `StandardGraph`,
      because a frame that always paid for a second pass over every instance and
      a readback nobody was looking at would be the wrong default. So the
      renderer had to be able to run a graph somebody authored, which is the
      thing `PIPELINE_NODES.md` stage 3 said the whole node editor was for.

      Refused rather than half-applied: a graph that does not compile, or that
      names a kind nothing can draw, leaves the previous pipeline running and
      says why.

**Two contradictions the building turned up**, both found by trying to run what
the catalogue described rather than by reading it:

- **`Validate` refused every sink.** It fired on any node with no writes, and
  `viewer` and `capture` both write nothing by definition — so two catalogue
  kinds could never be placed in a graph that compiled, and nothing noticed
  until something tried. What a sink produces is a panel or a file, outside the
  graph, which is exactly why the graph cannot see it. The rule is now "neither
  reads nor writes", which still catches the pointless node the original case
  was written for. Mutation-checked: putting the old rule back fails both the
  new sink case and the viewer pipeline.

- **A per-view node cannot be appended to the standard frame.** `Compile`
  refuses it with `SharedBetweenViews`, because the frame's `Frame`-scoped tail
  — `overlay`, `interface` — is already declared and a per-view node after them
  would mean "every view, then the panels, then every view again". Declaration
  order is the order, so an authored pipeline has to be *declared* in it rather
  than assembled by appending. Worth knowing before anybody writes the editor
  action that adds a node.

---

## Rules for this work

- **The golden image is the gate for anything touching `render`.** Re-run
  `just studio-smoke` and check the hash, every step.
- **`just check` green before ticking a box.** Not after the stage — after the
  step.
- **A test that fails to fail is not a test.** Every check added here was
  mutation-verified by breaking the thing it checks and watching it go red.
- **Findings go in the code, not only here.** Three came out of stages 1–7 and
  each is recorded where somebody would trip over it again: external resources,
  `Storage` being unsamplable, and our `Compile` refusing what Unreal's sorts.
