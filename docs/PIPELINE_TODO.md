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
| 3 | Executor — `Renderer::Render` runs the graph | **step 1 of 3 done** — the seam exists and is tested; the swap is not made |
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

### Stage 3 — the executor (D00002) · step 1 of 3 done

**The gate is the golden image.** `just studio-smoke` must still produce
`51931c790b836cb4d37275a276e52890`. That hash is the whole safety net for
replacing how the frame is submitted, and it is worth re-running after every step
rather than at the end.

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

      **Order to move them in**: `shadow` (smallest, no colour target, cleanest
      boundary), then `surface`, `opaque`, `transparent`, then `overlay` and
      `interface` — the last two sit *outside* the per-view loop and so need
      nothing from the per-view struct.

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
