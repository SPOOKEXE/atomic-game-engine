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
| 2 | Catalogue — 45 node kinds, typed and formatted slots | **done** |
| 3 | Executor — `Renderer::Render` runs the graph | **step 1 done, step 2 two passes of six** — `shadow` and `surface` are handlers; `opaque` is next and has a decision in it |
| 4 | Static checks — nine fault kinds | **done** |
| 5 | Authored order, and scopes | **done** |
| 6 | Access grid / profiler visualiser | **done** |
| 7 | Instrumentation | uploads **done**; GPU timestamps blocked on 3 |
| 8 | Readbacks — viewer image, histograms, overdraw | **not started** |

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

- [ ] **Step 2 — the extraction.** Each of the six pass blocks inside
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

      ### The golden image does not gate every pass

      **Found by mutation, and it changes how the rest of this stage is
      checked.** Making `SubmitSurface` return before submitting leaves
      `51931c790b836cb4d37275a276e52890` **unchanged** — the standard capture
      does not show a pane that samples a surface texture. `shadow` mutates the
      hash; `surface` does not, and no deterministic capture does: the Mirrors
      world only becomes the capture target after several hundred frames, by
      which point the capture varies run to run for `studio-meshes`' reason.

      So the golden image is necessary and **not sufficient**. What was used
      instead, and what should be used for the remaining four passes:

      **Probe the frame's own counters against the pre-change build.** A
      temporary `ENGINE_ERROR` at the end of `Render` printing `DrawCalls`,
      `Triangles`, `SurfacePasses`, `SurfaceInstances`, `Passes`, `Culled`,
      `Uploads` and `UploadedBytes`, run for 40 frames with no capture, is
      **deterministic** — two runs are byte-identical. Build it on `HEAD`, keep
      the log, `git stash pop`, build it again, `diff`. For 2a–2c that came out
      identical over 27 frames including `surfacepasses=4`, which is the check
      the hash could not make.

      It is worth keeping this as the routine for anything in `render` that
      claims to be behaviour-neutral: it costs two builds and it covers the
      passes the one deterministic capture cannot see.

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

- [ ] **Step 3 — the swap and the deletion.** `Render` calls `Execute` instead of
      walking `PassOrder()`. Then `PassOrder()`, the `Pass` enum and
      `PassRecorder`'s ordering guard all go — `Execute` is the ordering, so
      keeping a second one would be the third description of the frame that
      D00016 is about.

### Stage 7's other half — per-pass GPU timestamps (D00046)

Blocked on stage 3: there is nothing to put a timestamp around until the graph
is what executes.

- [ ] `SDL_GPU` timestamp queries around each node the runner executes
- [ ] Into `ProfilePass::Elapsed`, which the panel already shows as *not
      measured* while it reads zero

### Stage 8 — readbacks (D00047)

- [ ] A download path with fence discipline. A frame late is fine for a debug
      view and should be said out loud rather than hidden
- [ ] The `viewer` node, which is in the catalogue and does nothing
- [ ] Channel histograms — answers "is this alpha blank", fault 3
- [ ] An overdraw view — fault 9

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
