
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

### [_] D00004

- `Engine::core` links Crypto++ for `engine::core::Random`, and everything links `core` — so a SHA-256 implementation is in the client and the server alike. Measured: 9,479 `CryptoPP::` symbols in each, 36 of the archive's 173 members, because `cryptlib.o` is unavoidable and drags the algorithm registry behind it. `docs/CPP_LINKER.md` §2 has the mechanism.
- **The only callers today are placeholder scenes.** `BuildPlaceholderWorld` in `mono.server/src/Simulation.cpp` and the orbit demo in `mono.client/src/Demo.cpp`, both at spawn time, ~6 values per entity. `mono.client/AGENTS.md` already says `Demo.hpp`/`Demo.cpp` go away when there is a game file to load a scene from.
- So the question to ask when the demo dies: **does anything still need `Random`?** If the answer is no, `core` should stop linking Crypto++ and the notices in `THIRD_PARTY_NOTICES.md` and `mono.vendor/THIRD_PARTY_NOTICES.md` move Crypto++ back to test-runner-only. Written down here because nobody asks it otherwise — the library sits in the binary forever and the next reader assumes it is load-bearing.
- What `Random` is actually for outlives the demo, and is why this is deferred rather than reverted: a value identical on every machine, which `std::mt19937` plus `std::uniform_real_distribution` cannot promise. Procedural placement, replay, and anything a server and client must agree about want exactly that. If a v0.3 consumer appears, close this item and say so.
- The cheaper option, if no such consumer appears: keep `Random`'s interface and put a small specified integer mixer behind it. Same portability guarantee, none of the archive. The interface was designed so this is an implementation swap, not a call-site change.

### [_] D00003

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

- `--script PATH` is accepted and warns. There is no VM until v0.5, and a flag that is silently ignored is worse than one that says so.
- `core/types` has `Vector3`, `Color3` and `CFrame` only. The rest of the value types arrive with v0.3's Basic Components, where they have a consumer.
- `Column`, `ComponentSet`, `SparseSet` and `ChangeChannel` are not in `ecs` yet. They belong with v0.2's multi-world work, where the storage layout has to be something the engine controls rather than something flecs decides.
- macOS builds compile SPIR-V but not MSL; the cross-compile step is wired in CMake and untested. Linux/Vulkan is the verified path.
