
# DEFERRED

## Format

Each section has the header `[_] D00000`.

The numerical is a counter that increments every item.

Insert the NEWEST items to the front, so older are towards the back.

If a deferred item no longer exists, say the related code was deleted, then mark with [DELETED] flag.

```
### [_] D00101

- item 1
- item 2
- item 3
```

and for deleted marked items;

```
### [DELETED] D00001

- item 1
- item 2
- item 3
```

## Deferred Items

### [_] D00012

- **The serial/parallel crossover moved when `release` went to `-O3`, and every constant chosen against it was picked at `-O2`.** `Jobs::DEFAULT_GRAIN` is 4096 and `MINIMUM_GRAINS` is 8, so nothing below 32,768 rows is handed to the pool — a threshold argued for in v0.1 from a crossover measured at ~60-80k, re-measured at v0.2 after the storage rewrite, and now resting on a serial loop that is **twice as fast as it was when the number was chosen**.
- The evidence that it moved: `Each · 10k` is now 1.78 us and `EachParallel · 10k` is 2.48 us — the parallel path measures **17.6% slower at ±3%**, where before it was closer. Halving the serial side pushes the crossover up, because the pool's wake-up cost did not change. `EachParallel` at 100k and 500k also read slower, but at ±31% and ±61% those are noise and are not claimed.
- **This is the second time in one version that a crossover constant turned out to be describing a machine that no longer exists.** v0.4 already found `IntegrateMotion`'s crossover is ~4096 rows rather than 60-80k, because a row carrying a `CFrame` and a quaternion is far more expensive than the three-multiply-add body the original figure was taken on. One item moved the row's cost up; this one moved the loop's cost down. **A grain constant is a ratio between two things that both change, and nothing in the build notices when it goes stale.**
- What a real answer needs: the crossover re-measured at `-O3` for `Each`, `EachBatch` and `IntegrateMotion`, and then a decision about whether one `DEFAULT_GRAIN` can serve bodies that differ by an order of magnitude in cost — `physics` already carries its own `INTEGRATE_GRAIN` of 512 because the shared default was wrong for it, which is one data point for "no".
- Deliberately not done in the same pass as the flag change. Re-tuning a dispatch threshold on the strength of one afternoon's benchmarks is how the previous two numbers got there, and the honest move is to have the flag settled first so the re-measurement is taken against something that will not move underneath it.
- **Reopen trigger: the next `EachParallel` grain question, or the first system that measures slower parallel than serial in a real world.** The cheap check is already written — `engine.ecs.bench.iteration` carries both paths at 10k, 100k and 500k, so the crossover is a table away rather than an experiment.

### [_] D00011

- **A structural change lost *on the wire* is only repaired by the eventual re-snapshot.** v0.4 fixed the case where the server refused its own message locally — `Authority::Unsent` hands back what the transport would not take, and the known-set edits are rewound with the cursor. That covers refusals, which were the common case and the cause of a real bug. It does not cover a datagram that left the server and never arrived.
- **Why this is not the same as a lost value.** A lost *value* is self-healing: the entity is still in the client's known set, it moves again, and the next delta carries it — and if it never moves again, the unconfirmed-entry resend added in v0.3 catches it. A lost *creation* is not, because the known set on the server now says the client has been told about an entity it has never heard of, so nothing will mention it again. The client is missing an object and is acknowledging happily.
- What repairs it today is `ResnapshotAfterTicks`, which is a blunt instrument aimed at a different problem — it is for a client that has fallen behind, and a client missing one creation is not behind. So the repair happens eventually and for the wrong reason.
- **A real answer is a protocol change: structure has to be acknowledged, not just ticks.** A client would confirm the creations and destroys it applied, and the server would retire known-set edits against that confirmation the same way it retires values against `Applied`. That is a second acknowledgement channel and a bigger change than it sounds, which is why it is here.
- **Reopen trigger: the first packet loss on a link that is not loopback.** Every transport in the tree today either delivers or refuses locally; nothing in the test suite drops a datagram in flight. **The cheap thing to do first is make one that does** — a lossy transport wrapper would turn this from an argument into a failing test, and it is worth more than the fix until somebody has seen it happen.

### [_] D00010

- **A replicated world is not interpolated, so it judders at the server's tick rate.** v0.4 made the client draw the world it replicates, which is what exposed this: the demo world beside it is smooth because `PreviousTransform` and the frame alpha exist for exactly this purpose, and the replica has neither.
- **It cannot simply be given a `PreviousTransform`, and that is the whole point of the entry.** The two states worth interpolating between are not "last tick" and "this tick" of a local simulation — they are two *received* ticks, arriving irregularly over a link with jitter. Writing a previous transform on arrival interpolates between whatever two packets happened to land, which is smooth only while the link is.
- What it actually needs is **snapshot buffering**: hold received states in a small time-ordered buffer, render at a fixed delay behind the newest, and interpolate between the two that bracket the render time. The delay is the jitter budget and is the one number that matters — too small and it stalls on every late packet, too large and the world is visibly behind. That belongs in `replication`, beside the tick agreement, and not in the client.
- **It interacts with prediction and must not be built in ignorance of it.** The local player is predicted and must *not* be delayed; everything else is interpolated and must be. So the buffer applies per entity by whether it is predicted, which is a distinction `Prediction` already draws for its own reasons.
- **Reopen trigger: a client watching another client move.** Today the only replicated motion is a placeholder world's bounce, and one observer. The judder is visible now and tolerable; it stops being tolerable the moment two players can see each other.

### [DELETED] D00009

**Closed the same day it was opened, by doing it.** Kept rather than deleted, because the entry's own stated test is what settled it and that is worth being able to point at.

- The finding: `release` and `bench` compiled at `-O2`, and the ECS iteration control ran 100k rows in 38.74 us at `-O2` against **16.61 us at `-O3`**. Found sideways, while disproving v0.4's vectorisable-layout item — measured at `-O2` alone, the packed and padded layouts look the same and that item reads as merely unhelpful rather than backwards.
- The objection was floating point: `-O3` vectorises and inlines more aggressively, and this repository diffs two runs byte for byte. **This entry named the measurement that would settle it — `just determinism` and `just replay-check` at `-O3` — and both are byte-identical.** GCC enables neither `-ffast-math` nor `-funsafe-math-optimizations` at any `-O` level, so IEEE semantics never moved. The whole suite was also built and run optimised, which nothing in the presets otherwise does: `release` has `MONO_BUILD_TESTS` off, so **the shipping optimisation level had never had the tests run against it at all**, and raising the level is exactly what surfaces latent undefined behaviour. 104 suites, 18 `ctest` targets, all pass.
- Both places moved, and the second one is the one that would have rotted: first-party targets now state `-O3` rather than inheriting `RelWithDebInfo`'s `-O2`, and `mono_add_benchmarks` pinned `-O2` of its own. Those two had agreed by coincidence, not by construction, so the benchmark binaries would have gone on reporting the old number for the thing that ships. `MonoLibrary.cmake` now says to change them in one commit.
- **See `ROADMAP.md` v0.4 for the measured outcome**, which is not uniform: serial row iteration roughly halves, and a handful of structural and query-planning paths get 4-12% worse. And see `D00012`, which is the new question this opened.

### [_] D00008

- **The single-player `ALLOW_TIER_ESCAPE` in `mono.client/CMakeLists.txt`.** It is written out in a comment there and deliberately not declared: `DEPS ... Mono::server` plus `ALLOW_TIER_ESCAPE Mono::server`, the one edge the tier rule has to permit by name rather than by rule, so that a `client`-tier program may link a `server`-tier library.
- v0.3's roadmap listed declaring it as part of wiring the two programs together. **The wiring turned out not to need it, and that is the finding rather than an excuse.** `--connect` talks to a server in another process over a UDP socket, which is precisely the arrangement where the client links no server code at all. Declaring it now would add an escape with no user — which is what the comment itself says not to do, and what somebody would eventually reach for to do something unrelated.
- **Reopen trigger: a client hosting a server in its own process.** That is single-player, and it wants a game file to host before it is worth building — so `mono.gamefile` is the real prerequisite. When it lands, the edge is two lines and the comment already says which two.
- Worth keeping straight, because the two are easy to confuse: the escape is about *linking*, not about connecting. A single-player client that spawned `mono.server` as a child process and connected to it over loopback would need no escape either, and is a legitimate third option to weigh at that point — it costs a process and buys the same crash isolation `parallel/process` already argues for.

### [_] D00007

**The bandwidth half closed at v0.4. Lag compensation is untouched. They were filed together and should not have been — one had a trigger that could fire and the other has a trigger that cannot yet.**

- ~~Priority under a bandwidth cap.~~ **Closed, and the reopen trigger fired exactly as this entry wrote it.** `SendsOverBudget` came off zero in a real cross-process run: a 2000-entity world's tick was ~137 messages against a 64-packet budget, so 73 were dropped every tick with the tail chosen by position in a vector — the precise failure this entry predicted, found because the number it named as the signal was the number that moved. What shipped is what this entry asked for: a score per entity per client supplied by the game (this module carries named components and cannot know which one is a position, the same argument `SetInterest` already makes), **a rotation that outranks the score rather than being weighted against it**, and an explicit per-client answer with the reasoning in the header — the budget belongs to a link and there is one link per connection, so a per-server cap would have to be divided before it could be enforced, and that division *is* a per-client cap. The starvation bound is `StarvationTicks + ceil(n/k)` and is asserted by a test rather than argued for. Ordering costs nothing when there is no pressure: rows go out in dirty-bit order and are only re-packed by score if that did not fit.
- Worth keeping from the closure, because it was nearly missed: **the item was found by a bug, not by a measurement anybody set out to take.** The refusals were being blamed on load and on a wall-clock deadline for four separate investigations. The entry's own advice — "`ConnectionStats` already counts the refusals; read it before concluding a component is not replicating" — was right, and nobody read it. A counter that is not looked at is not a mitigation.
- **Still open: lag compensation** — rewinding the server to what a client saw when it fired. It needs a server-side history buffer of past ticks that `replication` deliberately does not keep, and a policy for how far back it will honour, which is a game-design decision about fairness rather than an engine one. **Reopen trigger: the first hitscan weapon**, which cannot be built without it. v0.4 brought the physics and the `Part` that trigger was implicitly waiting on, so the blocker is now the game rather than the engine.

- **Lag compensation** — rewinding the server to what a client saw when it fired. It needs a server-side history buffer of past ticks that `replication` deliberately does not keep, and a policy for how far back it will honour, which is a game-design decision about fairness rather than an engine one. **Reopen trigger: the first hitscan weapon**, which cannot be built without it and which nothing before v0.4's physics can express.

### [_] D00006

**Mostly closed at v0.4. What remains is narrower than what this entry was opened for, and is restated here rather than left implied by a struck-through paragraph.**

- ~~`replication::Listener` admits a client on its first datagram.~~ **Closed.** The three things this entry said were missing — a key exchange before a slot is reserved, a challenge answered before any state is allocated, and an answer to who may connect — are all in. `net::Handshake` is wired in on its own channel, so `Listener::Poll` routes by channel *before* by sender and a datagram from an unknown address on any other channel is dropped and counted. The challenge is **stateless**: an HMAC cookie over the peer's address and its key-exchange message, keyed by a rotating secret, so an unanswered challenge costs zero bytes and stays zero however many are outstanding — the failure this entry was really about was never the slot, it was that a stranger could make the server *remember* something. Proved by two hundred endpoints saying hello, nothing being allocated, and the first of them still answering afterwards. Admission is an injectable policy; the default admits anybody who completes the handshake and the header says so in those words, because a handshake proves a peer can receive where it says it can and do arithmetic, not that it is welcome.
- ~~An entity with no replicated components appears in the snapshot as a bare row.~~ **Closed.** The visible set is now built from entities carrying at least one replicated component.
- **Still open, and it is the thing "handshake" is most likely to be misread as covering: the stream is in the clear.** The two directional keys are derived, used to confirm the exchange with a Poly1305 tag over the associated data, and then destroyed. Encrypting the traffic is a wire-format change — a counter on the wire, the header as associated data, and the tag coming out of `MAXIMUM_PAYLOAD_BYTES`, which shrinks every budget in the module by sixteen bytes. Both `net/AGENTS.md` and `replication/AGENTS.md` now say the stream is unencrypted, so nobody reads the presence of a handshake as the presence of confidentiality.
- **Still open, and the reason the default policy's honest wording matters: nothing binds the exchange to a server identity.** `net::Handshake` carries its own `TODO(v0.4)` for this. Until it lands, the agreement is unauthenticated against a relay — a peer knows it is talking to *something* that completed X25519, not that it is talking to this server. A static server key and a signature over the exchange is the shape; where the key comes from and who trusts it is a deployment question, which is why it is here rather than built.
- **Reopen trigger, unchanged and still unmet: the first time this listens on anything but loopback.** The bound (`MaximumClients`, 64, counted in `Statistics::Turned`) is still in front of the handshake and is still what makes this a gap rather than a hole.

### [_] D00005

- **`.github/workflows/ci.yml` is deferred by decision, not by effort.** What was never committed is the file that makes a machine other than this one run the checks, and it is deliberately not going to be: a workflow on GitHub fires jobs, and this repository does not want jobs firing.
- **Correction, made at v0.4: this entry used to say "the checks it would run are written and pass", and that was half false for as long as it was written down.** It was true of `just check`, which defaults to the `dev` preset. It was false of `just preset=ci check` — the configuration this very entry names as "what the pipeline actually enforces" — which **did not compile at all**, because `ci` makes every warning fatal and two of them were live: a `-Wmissing-field-initializers` in `core::Arguments` and a `-Wdangling-reference` at five sites in `world`'s suites. Both are now fixed and the preset passes end to end. **The lesson is the one this file already records about `just docs-check` in v0.2** — a check nobody can run stops being read, and then stops being true, and the sentence claiming it passes ages into a false one. If a recipe is named here as the standard, something has to run it.
- So the guarantee today is **local and manual**: `just check` before a push, run by a person who remembers to. That is honest rather than green — it is the same guarantee the repository has had all along, now written down instead of implied by a roadmap line that read as pending work.
- **What is actually lost is the second machine, not the checks.** Two things only a different box can prove. The tier split: `just check-server-is-headless` and `just check-cdn-is-bare` currently pass on a machine that *has* a graphics stack, so they prove the binary does not link one — but a job on a box with no graphics stack at all would prove it by building there and succeeding. And the fresh-clone case: a check that quietly depends on something in this working tree passes here forever and fails for the first person who clones.
- The split a workflow should take, if one is ever wanted, is by **what each job needs installed** rather than by what it checks — that is what makes the headless job's environment the proof. `just check`'s list is the job list, in the same order, or "it passes here" and "it passes in CI" stop meaning one thing.
- **Reopen trigger: a second contributor, or a pipeline that is not GitHub's.** The narrower option that meets the objection directly is a `workflow_dispatch`-only workflow — it exists in the repository, runs nothing on push, and is started by hand when wanted. Recorded here because that distinction is not obvious later: the objection is to jobs running, not to the file.
- Removed from `ROADMAP.md` v0.2 rather than left unticked. The two items that once claimed CI existed were corrected before this was deferred, and both now read accurately: v0.2's recipe item says the recipes "exist and pass locally", and the determinism item describes `just check` as the local chain. Nothing left in the roadmap asserts a pipeline.

### [_] D00004

- `Engine::core` links Crypto++ for `engine::core::Random`, and everything links `core` — so a SHA-256 implementation is in the client and the server alike. Measured: 9,479 `CryptoPP::` symbols in each, 36 of the archive's 173 members, because `cryptlib.o` is unavoidable and drags the algorithm registry behind it. `docs/CPP_LINKER.md` §2 has the mechanism.
- **The only callers today are placeholder scenes.** `BuildPlaceholderWorld` in `mono.server/src/Simulation.cpp` and the orbit demo in `mono.client/src/Demo.cpp`, both at spawn time, ~6 values per entity. `mono.client/AGENTS.md` already says `Demo.hpp`/`Demo.cpp` go away when there is a game file to load a scene from.
- So the question to ask when the demo dies: **does anything still need `Random`?** If the answer is no, `core` should stop linking Crypto++ and the notices in `THIRD_PARTY_NOTICES.md` and `mono.vendor/THIRD_PARTY_NOTICES.md` move Crypto++ back to test-runner-only. Written down here because nobody asks it otherwise — the library sits in the binary forever and the next reader assumes it is load-bearing.
- What `Random` is actually for outlives the demo, and is why this is deferred rather than reverted: a value identical on every machine, which `std::mt19937` plus `std::uniform_real_distribution` cannot promise. Procedural placement, replay, and anything a server and client must agree about want exactly that. If a v0.4 consumer appears, close this item and say so.
- The cheaper option, if no such consumer appears: keep `Random`'s interface and put a small specified integer mixer behind it. Same portability guarantee, none of the archive. The interface was designed so this is an implementation swap, not a call-site change.

### [DELETED] D00003

- **Closed at v0.2 by the storage rewrite.** Every iteration path now goes through one cached `QueryPlan` per term list, topped up rather than rebuilt as tables appear, so nothing builds a query per call. The flecs-shaped problem below no longer exists — there is no `flecs::query` to be typed or untyped about.
- `Each` and `EachParallel` still build a query per call. `CountMatching` now caches its query and a typed cache for the iteration paths is the same idea, but it needs a per-store map of typed `flecs::query<Ts...>` rather than the one untyped kind, so it is a bigger change than the count was.
- Not urgent and not measured. Both iteration paths cost what they always cost — this is a saving, not a regression to fix — and the number to have before doing it is what query construction is as a fraction of a tick at a realistic entity count.
- Likely moot at v0.2, when `Column`/`ComponentSet` replace flecs as the storage and the query object stops being flecs's to build.
- Resources are per-world with no ordering guarantee against each other, which is fine while they are written by one system each. When two systems write one resource, that ordering is a phase question, not a resource question.

### [_] D00002

- The graph renderer of `RENDER_PIPELINE.md`. The current `render` module is stage 0 of its twelve — one instanced opaque pass and one overlay pass, standing in for stage 1's skeleton.
- Its stage 2 needs `ecs::ChangeChannel` for per-node cache invalidation, and its §4.2 needs `ecs::Column`/`ComponentSet` to store nodes as rows. Both wait for v0.2, which is D00001.
- The graph runtime itself is `mono.engine/graph/` at L9 and does not exist. `render` must not grow a hand-rolled pass list before it does.
- §12.3's remaining additions to F5: per-stage cache hit/miss, and the undemanded capability of a dead node. Both need the graph. The tick rate on F3 and the tick/render split of §14 are done.

### [_] D00001

- `--script PATH` is accepted and warns. There is no VM until v0.6, and a flag that is silently ignored is worse than one that says so. **Still open, and now the oldest thing in this entry.**
- ~~`core/types` has `Vector3`, `Color3` and `CFrame` only.~~ **Closed at v0.4.** `AABB`, `Ray` and `RayHit` landed with the consumers this bullet was waiting for — `spatial`'s queries and `physics`'s narrow phase. Nothing else was added, deliberately: `Vector2` was considered and refused because §3.4 gates it on "the overlay or editor needs it" and neither does, and the culling operations an `AABB` invites (`Inverted`, `Grown`, `Contains(AABB)`) have no caller until v0.6's frustum cull.
- ~~`Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are not in `ecs` yet.~~ **Closed at v0.2** by the storage rewrite, and reopened and closed again at v0.4 by chunking. Recorded here rather than deleted because this bullet is why the entry was still `[_]` after the other half of it had shipped.
- macOS builds compile SPIR-V but not MSL; the cross-compile step is wired in CMake and untested. Linux/Vulkan is the verified path. **Still open, and still the least examined line in this file** — it is the only item here with no trigger, because nobody has a Mac to trip it.

**Two of four bullets are now closed, and the entry stays `[_]` for the other two.** `v02v03v04.md` predicted this edit and said it belonged "with the next pass over `docs/DEFERRED.md`, not here"; this is that pass.
