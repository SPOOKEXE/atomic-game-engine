
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

### [_] D00008

- **The single-player `ALLOW_TIER_ESCAPE` in `mono.client/CMakeLists.txt`.** It is written out in a comment there and deliberately not declared: `DEPS ... Mono::server` plus `ALLOW_TIER_ESCAPE Mono::server`, the one edge the tier rule has to permit by name rather than by rule, so that a `client`-tier program may link a `server`-tier library.
- v0.3's roadmap listed declaring it as part of wiring the two programs together. **The wiring turned out not to need it, and that is the finding rather than an excuse.** `--connect` talks to a server in another process over a UDP socket, which is precisely the arrangement where the client links no server code at all. Declaring it now would add an escape with no user — which is what the comment itself says not to do, and what somebody would eventually reach for to do something unrelated.
- **Reopen trigger: a client hosting a server in its own process.** That is single-player, and it wants a game file to host before it is worth building — so `mono.gamefile` is the real prerequisite. When it lands, the edge is two lines and the comment already says which two.
- Worth keeping straight, because the two are easy to confuse: the escape is about *linking*, not about connecting. A single-player client that spawned `mono.server` as a child process and connected to it over loopback would need no escape either, and is a legitimate third option to weigh at that point — it costs a process and buys the same crash isolation `parallel/process` already argues for.

### [_] D00007

- **Priority under a bandwidth cap.** Interest management decides what a client *may* see. Nothing decides what to drop when what it may see does not fit the link's per-tick budget.
- **There is already a policy, and nobody chose it.** v0.3 made a tick's delta go out as several messages so it fits a datagram; `Link::Reserve` refuses the ones past the budget. So the thing that gets dropped is *whatever happened to be last in the component list*, which is deterministic — worth something — and arbitrary, which is the problem. The failure mode is a component starving forever because of where it sits in a vector, and it will look like that component being broken rather than like a budget being exceeded.
- The cheap mitigation, if this bites before it is designed: `ConnectionStats` already counts the refusals, so the symptom is visible from the server rather than only from the client's eyes. Read it before concluding a component is not replicating.
- What a real answer needs: a score per entity per client (distance, recency, whether the client is looking at it), a rotation so nothing starves, and a decision about whether the cap is per-client or per-server. That is a design, not a patch, which is why it is here.
- **Reopen trigger: the first world whose per-tick delta does not fit the budget.** Measurable rather than judged — `ConnectionStats::SendsOverBudget` moving off zero in a real run is the signal.

- **Lag compensation** — rewinding the server to what a client saw when it fired. It needs a server-side history buffer of past ticks that `replication` deliberately does not keep, and a policy for how far back it will honour, which is a game-design decision about fairness rather than an engine one. **Reopen trigger: the first hitscan weapon**, which cannot be built without it and which nothing before v0.4's physics can express.

### [_] D00006

- **`replication::Listener` admits a client on its first datagram, and that is not authentication.** Anybody who can reach the port takes a slot and is streamed the world. Written down here rather than left as a comment because "the handshake is not wired in" reads as a detail and "any stranger gets the world" is the actual property.
- What exists and is unused: `net::Handshake` — X25519 into HKDF-SHA256 into ChaCha20-Poly1305, verified against RFC 7748 §6.1 and RFC 8439 §2.8.2, with a `Sealer`/`Opener` pair whose nonce uniqueness is structural. It is finished. What is missing is the *policy* around it: a key exchange before a slot is reserved, a challenge the peer answers before any state is allocated, and an answer to who is allowed to connect at all. None of those are `Listener`'s to invent.
- **What is in place is the bound, and it is the difference between a gap and a hole.** `ListenerSettings::MaximumClients` is 64, and a datagram from a stranger past that is refused and counted in `Statistics::Turned`. So the worst an unauthenticated admit can do is fill the server, not exhaust its memory. A slot costs a session, a link, two reliability windows and a per-client known set, which is why that number is not one to raise casually.
- Also open, and smaller: the join snapshot filters *components* by the `Replicate` list and *entities* by the interest predicate, but an entity with no replicated components still appears in the snapshot as a bare row. That leaks a count, not data.
- **Reopen trigger: the first time this listens on anything but loopback.** A `--listen` on a public interface is the moment the bound stops being sufficient, and it should not be the moment somebody discovers this file.

### [_] D00005

- **`.github/workflows/ci.yml` is deferred by decision, not by effort.** The checks it would run are written and pass — `just check` chains format, build, every suite, the architecture check, the determinism diff and the replay diff, cheapest first. What was never committed is the file that makes a machine other than this one run them, and it is now deliberately not going to be: a workflow on GitHub fires jobs, and this repository does not want jobs firing.
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

- `--script PATH` is accepted and warns. There is no VM until v0.6, and a flag that is silently ignored is worse than one that says so.
- `core/types` has `Vector3`, `Color3` and `CFrame` only. The rest of the value types arrive with v0.4's Basic Components, where they have a consumer.
- `Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are not in `ecs` yet. They belong with v0.2's multi-world work, where the storage layout has to be something the engine controls rather than something flecs decides.
- macOS builds compile SPIR-V but not MSL; the cross-compile step is wired in CMake and untested. Linux/Vulkan is the verified path.
