
# ROADMAP

## Editing

Deferred items are for items that need significant systems we cannot do now.
If you can do the item now, do NOT add to the deferred list.

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. If a TODO item is not FULLY completed, split the
TODO item, keeping the concise short dash point list with block infront and
split it as another item under the same version.

For example, if we complete A and B but not C and D:
```
### v0.5
- [x] do: A1, A3, A5
- [_] do: A, B, C, D
- [x] do: G, H, I
- [x] do: K, K2, K3
```

becomes:
```
### v0.5
- [x] do: A1, A3, A5
- [x] do: A, B
- [x] do: G, H, I
- [x] do: K, K2, K3
- [_] do: C, D
- [_] deferred `D0001` for later.
```

or defer to another version.

## VERSIONS

The milestone headings below are development labels. Not in line with project versioning.

### v0.19

- [x] quic implementation. **Built, proved and wired behind `--quic`.** ngtcp2
      v1.25.0 is vendored `lib/`-only, the packet protection is
      `net/quic/Crypto.hpp` checked against RFC 9001 Appendix A, and the
      handshake is a TLS 1.3 stack in tree. `docs/QUIC.md` §0 is the staging
      table and §12 is what the survey got right and wrong; `docs/DEFERRED.md`
      D00014 is closed.

      **The TLS decision went to the fallback**, which §4 ranked second. AWS-LC's
      whole case rests on its pre-generated build files needing no Go and no
      Perl - a claim §11 lists as an open question and §10 says to verify on all
      three platforms *before* building on it. Verifying it on one proves
      nothing about the other two. So the backend is X25519, two suites, Ed25519
      and RFC 7250 raw public keys over Crypto++, and the fresh-clone rule
      survives unargued-with. **What it costs is interoperability and therefore
      HTTP/3**, which is the cdn half of D00014's second-consumer argument and
      is now the one thing that got worse.

      **`docs/CODE_ARCH.md` §10.1's open question is answered and it is the
      first of the two**: `replication::SessionPort` with two implementations,
      not a `Session` whose four members become optional. That would be one
      class with two modes and a branch in every method that only one
      configuration runs, which is §8's "two overlapping reliability stacks is
      worse than either" applied inside a type. Channels are sender-owned
      unidirectional streams, one per `MessageKind` - §10's table read literally
      - so a join snapshot and a door opening stop sharing an ordering.

      Three things survive the swap: `BytesPerTick` as a ceiling above the
      controller, Copa itself (deleting it is a decision to take Cubic's latency
      and belongs in a commit that says so), and `ConnectionStats` refilled from
      ngtcp2 with `SendsOverBudget` keeping its one meaning.

      Two findings the survey could not have carried. **ngtcp2 does not copy
      stream or CRYPTO data** - it holds pointers until the peer acknowledges,
      so the outbound buffers are a deque that is never re-seated and a vector
      would corrupt a retransmission rather than crash. **A server cannot encode
      its transport parameters until it has read the client's** - RFC 9368's
      version information is filled in when the remote parameters are decoded,
      so parameters encoded beside `ngtcp2_conn_server_new` carry a chosen
      version of zero and the far end calls them malformed with nothing saying
      which end was wrong.

      The one real bug was §10's prediction exactly: Crypto++'s `ChaCha` is the
      64-bit-nonce original where RFC 9001 §5.4.4 wants the IETF variant's 96,
      and the published vectors caught it where "the handshake does not
      complete" would have said nothing.

      Verified over the loopback, over a link losing fifteen percent, over two
      real UDP sockets, and end to end: a headless client joins a `--quic`
      server with 4097 entities. 362 suites green, `just determinism` and
      `just replay-check` byte-identical.
- [x] quic: retire the datagram stack. **Retired as the default, kept as a
      mode, and the deletion is now off the table rather than pending.** QUIC is
      what a server serves unless told otherwise: `mono.server --transport
      quic|datagram|both`, defaulting to `quic`. The client has no transport
      flag at all - `--quic` is gone from both programs, and `--transport`
      replaces the server's boolean rather than aliasing it, because nothing in
      the tree depended on the old spelling and a boolean beside a three-valued
      flag has no defined answer when somebody passes both.

      **One UDP port carries either, and the discriminator is one bit.** A QUIC
      long header - which every Initial is, RFC 9000 §17.2 - sets bit 7 (Header
      Form) and bit 6 (Fixed Bit), so an Initial's first byte is `0xC0..0xFF`;
      a `net::Packet` opens with `Packet::MAGIC` little-endian, so its first
      byte is `0x41` and bit 7 is clear. `net::WireOf` is that test and
      `net/Wire.hpp` writes the bit positions down. A QUIC *short* header is not
      separable this way - `0x40..0x7F` contains `0x41` - and does not need to
      be: a 1-RTT packet is routed by its destination connection id before
      anything asks.

      **Both refusals are explicit, so a fallback costs one round trip and not a
      deadline.** A datagram-only server answers a QUIC Initial with a Version
      Negotiation packet listing one reserved `0x?a?a?a?a` version; that shape
      was chosen over a CONNECTION_CLOSE because it is stateless, needs no keys
      - a CONNECTION_CLOSE would make a server that does not speak QUIC derive
      Initial secrets to say so - and is 27 bytes against a 1200-byte Initial,
      so it cannot be amplified off. A QUIC-only server answers a datagram hello
      with `AdmissionKind::Refuse` naming the wire it does serve, in 29 bytes
      against the hello's 60. Both keep `net/AGENTS.md`'s rules for answering a
      stranger: nothing remembered, reply smaller than the question.

      The client tries at most two transports, logs which one it landed on and
      why, and falls back on an explicit refusal immediately or on
      `AttemptSeconds` of silence. That deadline applies to the last attempt
      too, so a wrong address is a reported failure rather than a client that
      retries for ever.

      **§11's last question is answered and it had the wrong shape.** Discovery
      keeps its own identity and hands over no connection id either: a
      `SessionId` lives as long as a host and a QUIC connection id lives as long
      as one connection - and an endpoint holds several and rotates them - so
      announcing one would let a re-announcement invalidate a live session.
      `network/AGENTS.md` already forbids the coupling, and there is nothing to
      hand over anyway, because the handshake picks the ids and the server
      chooses the one the client addresses it by. What the advert gained instead
      is `Advert::Transports`, so a client that finds a datagram-only server in
      a browser pays no refusal round trip at all.

      `mono.studio` hosts and joins on QUIC by default through the same
      `SessionPort` seam, and `mono.unified_tests` gained `quic` and
      `quic-lossy` on its transport axis - twenty arrangements where there were
      twelve, with `Crossing` now driving `SessionPort` rather than a bare
      `Session`.

      **`docs/QUIC.md` §8's step 6 is closed as will-not-do.** `Packet`,
      `Reliability`, `Handshake`, `Cookie` and `ConnectionId` are what
      `--transport datagram` runs on and where the fallback lands, so deleting
      them would remove a mode an operator selects. `net/AGENTS.md` keeps the
      standing rule that a change to one stack is a question about the other.

- [x] check if we need to move files / classes / structures around. **All five
      worked through, and two of the five were refused with measurements** -
      `docs/ARCH_REVIEW.md` §C. `nodegraph` moved to `mono.studio/nodegraph`,
      and the part worth more than the directory is that its row lost its
      `layer` rather than keeping L11: the program band has no layer and
      `CheckTargetGraph.cmake` refuses an edge from anything that has one to
      anything that has not, so an engine module linking it now fails by name.
      Proved with a probe expectation. `Renderer::RenderView` split by node
      family; `script` became four modules; `mono.client` declares the three it
      used through public headers. **`SurfaceCameras.cpp` does not lift**: it is
      2,909 lines rather than 4,108, a figure it has never held across any of
      its four commits, and the edges are a cycle - five places in `scene` call
      in, one of them through a public header signature, while the file reads
      back down into twelve `scene` types. **`mono.libraries/` was recounted
      after all of it and is still two leaves against a bar of three.**
      Separately `spatial::CollisionGroups`' `@tier L2` tag was the error rather
      than the placement, since the header includes `spatial/LayerMask.hpp` and
      could not sit at L2 whatever the tag said.
- [x] clean up all ECS components. **The survey's ten clearest arguments were
      re-verified before being acted on and four were wrong**, each the same
      way: a component's `sizeof` read as what it costs a file.
      `physics::PhysicsWorld` writes five bytes, not forty members;
      `effects::Trail` writes 82, not 1068; `effects.` under a shared prefix
      would have failed every join, because a replica registers the module
      nowhere and `LoadSnapshot` refuses a component the build does not have;
      and `Visual::Locked` is a public `BasePart` property in both scripting
      languages rather than an editor flag. What was real: `scene.PortalTransitSeen`
      replicated, so the authority's transit serial defeated every client's own
      portal snap and a body crossing a seam interpolated between rooms instead
      of arriving; `scene.TextContent` and `scene.ShaderSource` carried
      hand-written serialisers and were dropped without a word by a gate that
      tests `Trivial` rather than `Serialisable`; `gui.Canvas` was derived from
      the local viewport three lines from `gui.Resolved` and only one of the two
      was excluded; and **`scene::Visual::Surface` had been written and read at
      eight bits since the field was widened to sixteen at v0.17 to lift a
      127-mirror ceiling**, so every slot past 127 was truncated into every
      save. `gui.Gradient` replicated 672 bytes to carry 56 and now writes 77.
      `scene::QuickHash` and `Humanoid::Radius` are gone, both with no reader
      anywhere in the tree; `scene.Transient` is a tag. **The padding rule §D
      claimed was checked was not checked**, and now is. The full survey, 35
      fields and 7 merges and 14 splits and 12 renames with a decision on every
      one, is §D5.
- [x] scan through all serial loops for parallel or vectorised work. **The first
      one was not a parallelisation and the rest were measured before being
      touched** - §F. The client ran eight full store walks per world per frame
      and discarded them; gated on the world's tick, `content.demand` went from
      0.028 ms mean over 20,000 parts to 0.001 ms, running on 18 frames of 400
      instead of 399. The ECS change channel is not what gates it, and the three
      reasons are written down: `ClearChanges` runs at the start of a tick, an
      out-of-tick write is cleared before any listener sees it, and
      `ChangeVersion` moves for every `Transform` in the archetype.
      `CollectReplicated` gained the profiling span it never had and **stays
      serial with the number written down**: 77 ns a row against a 7.74 us
      handover puts the crossover near 400 rows, which it clears by fifty times,
      and what actually stops it is `SnapshotBuffer::Sample`'s statistics and a
      filtered `push_back`. The three fetch-path costs inside the tick barrier
      are gone: the delivery cache walked its whole directory on every store, so
      caching N assets cost N squared stats, 1.17 ms a store down to 16.4 us;
      `Manifest::FindByRoot` was a scan that `SliceOf` called per member,
      11.96 us down to 319 ns with an index that holds no fact the asset list
      does not; and `ChunkStore` hashed every chunk twice, 14.76 us down to
      9.17 us a chunk.
- [x] update AGENTS.md in root and subdirectories. **All nine of the remaining
      false files are true**, plus the two documents and the generated page §B
      named. `parallel` was wrong twice and understated: `process/` and `ipc/`
      have existed since v0.2 and **neither was ever a directory**, and the
      join's serialisation point citation named a line the file has never
      contained. `launcher` undercounted: nine decisions had crossed into
      `Interface.cpp`, not six, three of them duplicating a filter loop the tab
      beneath ran again, and they moved into `Plan.hpp` rather than the rule
      narrowing, because unlike the `game` and `scene` precedents the launcher
      already had a home for them. `README.md` says six rules.
      `docgen/pages/Modules.md` is **generated** now rather than corrected, 41
      pages against the sixteen it carried. `docs/QUIC.md`'s L2 claim did not
      reproduce; it was already right at HEAD.
- [x] improve build times. **The whole of §E2's ranked list, in its order.**
      `-g1` on first-party objects in `release` and `ci` took the eight heaviest
      translation units from 60.3 to 41.6 CPU-seconds with objects 4.7 times
      smaller; `-g0` was measured too and rejected, because it buys 1.2
      CPU-seconds more and costs every backtrace its file and line. The unity
      build is worth **486 CPU-seconds down to 201** over the 45 library
      targets, and cost 81 anonymous-namespace renames across 24 modules rather
      than the nine estimated, all renames and none by widening linkage. **Test
      binaries stay one file per translation unit**: `TEST_SUITE_ID` declares
      two fixed-name objects per file and `TEST_DEPENDS` reads one by that name,
      so a suite is a file and renaming cannot change that. Precompiled headers
      measured at 10.4%, which ranks them below unity builds rather than above,
      so a preset gets one or the other. Vendor includes are all SYSTEM through
      one function that replaced six copies of the same block, **measured at no
      compile-time effect at all** and kept for `ci`'s `-Werror`. `just build`
      no longer reconfigures every time and the shader staging declares its
      outputs, so a null build is 0.08 seconds against 1.15. **The glslc entry
      attributed its cost to the wrong thing**: compiling all 28 shaders is
      2.3 CPU-seconds, and the 600 it names is the 1104 of building glslang and
      SPIRV-Tools, which the compiler cache already collects, so a second cache
      layer would save 2.4 seconds a build directory and could hand back a stale
      `.spv`. Separately `AABB` stopped being 99.8 percent `CFrame`, 73,835
      preprocessed lines to 25,368 across 152 objects, and `client/Settings.hpp`
      174,863 to 65,853.
- [x] check all asynchronous and parallel points. **Audited, and §F's own count
      was wrong**: `net`, `replication` and `mono.network` own **one**
      concurrency primitive between them, a mutex in the loopback transport, not
      seventeen across five files. §F3 is the full table of that one plus the
      fifteen process-wide primitives the networking path takes without owning.
      What §F2 called a process-wide mutex blocking `Authority::Publish` is
      `ecs::Components`' registry guard, taken by the `Find` and `Describe` that
      `BuildComponents` ran per declared slot per client - nine thousand
      acquisitions on a two-hundred-client tick, and never held across the loop.
      `Survey` resolves it once a tick, which is what made the loop spreadable
      at all, because `EachChangedRuns` calls `Store::RequireOwningThread` and
      aborts rather than races. Above eight steady-state clients it publishes
      through one lane per worker: 392 microseconds a client serial against 54
      through lanes at sixty-four clients, 486 against 79 at two hundred.
      Ordering is structural rather than disciplined, and
      `engine.replication.publishlanes` requires the two to send identical
      bytes. **Two more defects came out of reproducing it**: nothing bounded
      how many join snapshots one publish built, so a ten-thousand-entity world
      could not admit thirty-two clients at all, and `parallel::Jobs` destroyed
      its condition variables with workers parked on them, hanging `exit` for
      ever in any binary that started a pool and did not stop it.
- [x] add more MCP integrations. **The surface stopped being a window onto one
      running program.** `layer_table`, `module_get` and `module_may_link`
      answer out of `expected_graph.json` compiled in at configure time, so the
      verdict on "may `render` link `script`" is the one `just test-architecture`
      enforces rather than a second description of it, and `module_get` carries
      the reverse index, which is in no file. `class_list`, `class_get` and
      `script_check` cover the script side; **checking is offered and evaluating
      is not, and the reason is the tick rather than security**, because a chunk
      with a `while true` in it would hang the program from a thread there is
      nothing to interrupt from. `log_tail`, `log_level` and `metrics_read` ask
      the process what only the editor's panel could be asked. `test_run` and
      `test_result` return a handle, because a full run is 125 seconds. The
      protocol gained `resources/*` and `prompts/*`: 44 resources on a server
      including every `AGENTS.md` in the checkout, and 5 prompts rendered from
      `.claude/commands` rather than copied. `mcpbridge` had no tests and now has
      two suites, one of which spawns the real binary and speaks JSON-RPC through
      it; **writing them found three bugs**, including a well-formed JSON array
      that escaped `Surface::Answer` as an exception into the frame loop, and a
      report reader that called a run with five failed suites green.
- [x] build out a full logging, metrics, etc so we can track what the engine is
      doing in dev builds effectively. **A disabled log statement now evaluates
      nothing**, and all 711 call sites were swept twice and independently for
      arguments with side effects before the guard went in; none had one, so it
      landed with no call site edited. Every statement carries a category
      supplied by `mono_add_library` as the module's own name, which is how 711
      sites gained one with zero edits. Levels are per category and settable
      without a recompile; below a compiled floor a statement is removed by the
      preprocessor, **verified by looking for the trace literal in the object
      file rather than by trusting it**. Measured at `preset=bench`: under a
      nanosecond disabled with or without an argument, 84 ns enabled into a null
      sink, 20 ns throttled while quiet. `core::Metrics` grew the read side it
      never had, with the rule restated as the one it always meant, read to
      report and never to decide. **The four counter implementations resolved as
      three keepers and one duplicate**: `FrameGraph` needs tree structure,
      `HeapProfile` runs inside `operator new` and would recurse through
      `Metrics`' mutex, and the scheduler's timings are drained in system order
      by a panel that draws them in that order. `ENGINE_ASSERT` and its three
      siblings exist, where the tree previously carried exactly **one** C++
      `assert()`. All thirteen silent modules gained instrumentation, 178
      statements and 30 metric writes, measured at 240 added lines across a full
      `test-all`; `nodegraph` deliberately gained none and reports an
      unevaluable node through `NodeStatus::Note` instead.
- [x] think plan for future features as well listed in roadmap and plan for them
      now. **Ten component types declared, registered and tested with nothing in
      the engine reading them**, which is decision 16 rather than a gap, and
      which had to happen now for a mechanical reason: `Components::Seal()` runs
      at start-up, so a type that registers lazily aborts the first host that
      meets a game file carrying one. **§D4 was wrong about fog**: it has
      existed on the `Lighting` service since v0.16 and reaches a uniform, so a
      `scene::Fog` would be the second answer rule 2 refuses. `Constraint` is one
      generic six-degree-of-freedom joint with seven classes differing only by
      the prototype row `Instance.new` copies, and deliberately has no `Kind`
      field, because one would make a `HingeConstraint` whose axes are all free
      expressible. `LevelOfDetail` stores pixels of projected area per triangle
      rather than a distance, because decision 19 says selection targets quad
      utilization and a distance ladder answers differently for a tower and a
      coffee cup at one range. `Terrain` stores the recipe and never the ground.
      `docs/FUTURE_COMPONENTS.md` is what each gets wired to and in what order.
- [x] make the four unchecked architecture rules checked. `mono.tools/sourcecheck`
      reads first-party C++ as text, 1,413 files and 137 registered components,
      and decides four things the target graph cannot see. Getting rule 2 down to
      something a build can fail on took one discriminator - **a record that
      declares a function outlives a call and a plain aggregate is an argument
      list** - which cut the findings from eighteen to seven, and qualified-name
      resolution removed the twelve that were two modules sharing the noun
      `Entry`. **Half of rule 4 turned out to belong to the compiler already**:
      `Name`'s only conversion operator is an explicit one to `bool`, so
      `WriteUInt32(name)` has never compiled, and what is left is the spelling
      somebody reaches for when it does not. Three rules gate and
      `public-header` reports, because an unwired subsystem is not dead code.
      Thirteen fixture trees run before the repository does, and the runner
      holds exit status to the gating table, so a rule that found something and
      let the build pass fails the same way as a rule that found nothing.
- [x] finish the `AGENTS.md` sweep. Folded into the AGENTS.md item above; all
      nine files, both documents and the generated page are done.
- [x] call `Components::Seal()` at start-up in every program. Already true in
      the tree when this pass began - `mono.client/app/main.cpp:390` and
      `mono.server/app/main.cpp:338` - and re-verified. The three programs that
      deliberately do not seal each have their reason recorded in §D1.
- [x] register `WorldTime`, `PortalProxy`, `NotArchivable` and `DirtyBits` under
      explicit names. Already true in the tree when this pass began, along with
      `Sun` and `PoppercamState`, and re-verified. Naming `scene.PortalProxy`
      switched a live rule back on: `replication/src/Defaults.cpp` had tested for
      that exact string since portals landed and never matched.
- [x] decide where `persistence` and `ledger` go before something needs them.
      **`persistence` at L3 beside `ecs`, `ledger` at L5 beside `collision`, and
      nothing is renumbered**, because a layer is a ceiling and not a slot - the
      built table already carries six modules at L12 and six at L11.
      `persistence` is L3 for what it may see rather than what it contains, and
      **the load-bearing half is that it must not see `ecs`**: a datastore holds
      what a game chose to persist, and the moment those two can name each other
      the save format and the datastore format become one migration. Being below
      L9 is the constraint that actually binds, because `DataStoreService` is a
      script service. **Remote backends do not live there**: a remote store needs
      `net` at L11, so the port is low and the adapter is wherever it has to be,
      exactly as `net::Transport` already does it. `docs/CODE_ARCH.md` §4.2.
- [x] the three small structural fixes with no argument against them. All three
      done: `mono.client` declares `Engine::assets`, `Engine::graph` and
      `Engine::script`; `nodegraph` is in `mono.studio` with no `layer`; and
      `NodeCatalogue::Find` carries the contract `All` had. The half worth
      writing down on the last one is the **second** failure mode rather than
      the first: because `Specs` is sorted, a later `Register` can leave an old
      pointer naming a different kind **without dangling**, which looks entirely
      correct at the use site.
- [x] split `Renderer::RenderView`. It is 60 lines now and `Renderer.cpp` is
      1,702, down from 13,680. The state the handlers closed over is
      `ViewRecording`, whose members **are** the locals, so nothing is published
      from a local into a member and the moved code stayed textually identical -
      which is what let the split be verified mechanically rather than by
      reading. **Three of the eighteen `SDL_BeginGPURenderPass` calls were error
      strings naming the function**; of the fifteen real ones, thirteen are
      inside a node's execution and the two that remain cannot move: the host
      chrome pass is recorded after `output-image` so captures hold only the
      game image, and the clear of a window no node reached has no node to live
      in. Compiling the module at `-j24` with the cache and unity off went 11.0
      to 6.5 seconds wall, and the slowest unit in it 10.6 to 3.8. **§E2's 22
      second estimate rested on a stale measurement**: `Renderer.cpp` was 31.2
      seconds under 420 percent of stolen CPU and before E1 landed, and measures
      10.6 today. Five deterministic scenes capture byte-identically.
- [x] the rest of the measured build wins. Folded into the build-times item
      above; §E2 items 2, 3, 6, 7, 8 and 9 and both applicable §E3 headers are
      done, and §E3's `client/Scene.hpp` half is measured and refused at 9.7
      percent rather than the sixty-times it read as, because both hubs that
      carry it already name `render/Renderer.hpp` themselves.
- [x] the four correctness findings that are open. All four, plus two more of
      the same kind found while reproducing them. The JavaScript microtask drain
      is bounded by a **count** rather than a deadline, so replay stays
      byte-identical; the reproduction is the interesting half, because a
      reaction that queues a reaction ran **52 million jobs against 31,201
      interrupt polls in two minutes**, QuickJS polling once per ten thousand
      safepoints, so the step budget was never going to catch it. Both interrupt
      handlers stopped zeroing on trip. The client's copy of `scene::InputState`
      is gone and `InterfaceWorld()` is stated to be the input focus.
      `CommandQueue::Post`'s return is checked at all fifteen sites against a
      contract in three classes. `NodeCatalogue::Find` carries its lifetime.
      Added: `parallel::Jobs`' exit hang, and an upward edge from `script` into
      `scriptluau` that only linked because no header declared its caller.
- [x] the eight-walks-per-world-per-frame content scan. Folded into the serial
      loops item above.
- [x] a component catalogue and a module graph in the MCP surface. Folded into
      the MCP item above. **The port finding was wrong as stated and the real one
      was worse**: `core::Arguments::Value` requires a value, so a bare
      `--mcp-port` is a parse error and no default is reached, but
      `studio/Config.hpp` seeded the editor's saved preferences with 8720 while
      its help, `.mcp.json` and `RUNNING.md` all said 8738. `mcpbridge`'s
      CMakeLists reads the default out of the header at configure time and fails
      the configure when either disagrees.
- [x] the closing pass, which found three more things nothing was looking for.
      **A body's spin now goes through a portal with the rest of it.**
      `scene::Motion` carries a `Linear` and an `Angular`, the solver writes both
      and `physics::Advanced` integrates both, and `CrossPortals` mapped the
      first alone for four versions. It survived because the case is invisible
      in a wall: a seam's destination is built from the far pane's normal with a
      canonical up and the half-turn is a yaw, so any two upright panes compose
      to a map that leaves the world's up fixed. Put a hole in a floor, or
      tumble a crate end over end through a corner, and it does not. `Rotate`
      and not `Carry`, because radians per second carry no length: a hole that
      halves a body halves the radius it spins at and halves the speed of every
      point on it, so the rate those two divide to is unchanged, and `Carry`
      would spin a crate down to nothing over a few crossings of a shrinking
      pair. Reproduced twice before it was fixed, once against a pane pointed at
      the sky and once on the real `Portals-1-world` scene. The interaction
      beside it turned out to be a **state rather than a second bug** and now
      has the test that says so. `scene::NearestSeamDistance` lost its last
      caller when `ResolveActiveCamera` went and is kept as a decision-16
      surface with the argument in the header. And **documentation coverage is
      back to zero gaps**: fixing the malformed-comment pass unhid the coverage
      pass behind it, which reported 23 public entities without a comment, most
      of them surface this pass itself created.
- [x] the em dash ban is checked. Not on the original list, and it belongs here
      because root `AGENTS.md` bans the character in capitals and rule 6 says a
      rule the build does not check is documentation. `mono.studio/nodegraph`
      carried 157 inherited with the library and `DemoNodes.cpp` fifteen more;
      all 172 were rewritten by restructuring the sentence rather than
      substituting the character, five of them user-visible strings.
      `just em-dash-check` greps 1,722 first-party files in 38 ms, **including
      the seven files agents may not edit**, so the document that bans the
      character is held to its own rule. It refuses to go green on an empty scan
      three ways, and both failure directions were exercised.

---

- [_] explore the idea of having the active scene entities resident on the gpu always and we just have a compute timer on the gpu 24/7. this way, when the scene changes, we tell the gpu what changed. also doing a 2-way sync is easy with signatures/hashes with cpu-gpu, this way we have no swapchain waiting, we just compute at a given interval. sort of like "replication" to the gpu. same for particles, ui, entites, studio, etc.

### v0.21

- [x] thoroughly implement every user-interface element, including `SurfaceGui` and `BillboardGui` - `SurfaceGui` gains `ZOffset`, `MaxDistance`, `ClipsDescendants` and `Active`, and `BillboardGui` gains `Active`, `Brightness`, `ClipsDescendants`, `CurrentDistance`, `DistanceStep`, `ExtentsOffsetWorldSpace`, `SizeOffset` and `PlayerToHideFrom`; new classes `UIGradient`, `UITableLayout`, `UIPageLayout` and `UIDragDetector`; `ScrollingFrame` completed with `ScrollingEnabled`, `AutomaticCanvasSize`, the two `ScrollBarInset`s, `VerticalScrollBarPosition`, `ElasticBehavior`, the three bar images and `AbsoluteCanvasSize`/`AbsoluteWindowSize`, plus wheel and thumb-drag input; `RichText`, `MaxVisibleGraphemes`, `ContentText`, `TextBounds` and `TextFits` on every text class; `Interactable`, the four `NextSelection*`, `SelectionOrder` and `SelectionImageObject` on `GuiObject`; `HoverImage`, `PressedImage` and `ResampleMode` on the image classes; `Enabled` and `ApplyStrokeMode` on `UIStroke`. Laid out, drawn by both backends, saved, replicated, bound and in the Properties panel. `D00129` carries the members that need a subsystem this engine has not got (filed as `D00120`, renumbered at v0.17 - that number was already a retired entry)
- [_] build out all remaining roblox surfaces with available underlying surface
- [_] port many particle features from unity to here (https://docs.unity3d.com/6000.5/Documentation/ScriptReference/ParticleSystem.html)
- [_] `~/Documents/GitHub/BLADEBORNE_UNIFIED/game` port and also studio place `~/Documents/Bladeborne Floor 0.rbxl`. Turn this into a demo file.

### v0.22

- [_] find a way to (easily) and thoroughly test rendering steps and ensure they produce the right image with right projections
- [_] finish portals so lighting, physics, projection, clipping and geometry crossing the seam are seamless
- [_] ensure per-mesh render capabilities, global lighting render capabilities, camera lighting render capabilities, etc. compute shaders, post-processing, etc.
- [_] simplify and strip old rendering code that is not part of the node system. Everything should be in the node system.
- [_] port semi-real raytrace and path-trace as part of nodes
- [_] make demo render pipelines with semi-real raytrace and path-trace
- [_] (dynamic) ambient occulusion, screen-space, fog, atmosphere, clouds, global illumination, displacement maps (make it rendering only but not physical)
- [_] render pipeline nodes for above
- [_] plan the entire rendering system to a visual compositor system like Unity. https://docs.unity3d.com/Manual/scriptable-render-pipeline-introduction.html https://docs.unity3d.com/Packages/com.unity.visual-compositor@0.27/manual/nodes.html
- [_] viewport indictator direction gizmo (select and lock to certain directions)
- [_] 3d cursor and camera orbit options under gizmo
- [_] ensure full parallel/vectorised (i.e. get all active scenes => build entity list => update gpu resident => batch render all cameras in every scene)

### v0.23

- [_] build out default plugins (move all topbar tools and stuff to plugins as a "Default Studio" plugin)
- [_] build out plugin function suite (create dropdown, edit toolbar, edit viewport, edit script editors, etc)
- [_] universe shared assets folder and setup easy cdn with it (when you load the universe file, it sets up a cdn with it).
- [_] add a universe loading widget - shows cdns the universe has and asks to allow permission, also http enabled property if changed
- [_] add tabs to the universe importer: general, assets, permissions, cdn, misc with all or per-world breakdown

### v0.24

- [_] default R6 base character (capsule collider)
- [_] gtlf default character (unreal)
- [_] make humanoid a shim for character controller (so not a black box), loads a default one in
- [_] character controller + humanoid + character states + state controller + bone controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] animation handler
- [_] skinning and animation - `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet
- [_] add accessories support

### FUTURE

- [_] unity porting tools / unity shop
- [_] roblox porting tools (rbxl) - in the widget that pops up, show all asset ids and make a assets selector so you can click which asset id points to which file asset (same for animations and whatnot where possible).
- [_] (procedural, node-based) terrain generator (refer to discord references)
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) - untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds. Will show a widget that tells you conflicts and missing classes.
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms.
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] embedded whiteboxing tools (planning)
- [_] full procedural terrain studio tools
- [_] full ui features
- [_] level-of-details (4 different meshes version, auto-decimate version, smart-triangle-reduction-version thinking of nanite triangle surface area)
- [_] project demos: space engineers asteroids + planets full demo, blackhole simulator (warp space, warp visual, etc), huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot, user interface (copy bladeborne's for demo?)
- [_] datastores (sqlite, mongo, supabase, etc - make a selection with local and remote setups)
- [_] html-based ui creation (html-script?)
- [_] import blender files in asset explorer
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful.
- [_] smart platform backend where only "admitted keys" can connect to a given server - i.e. whitelist-based servers (press play on website => generate play key => platform tells server user is connecting with key => send key + server to user => user connects to server using key and info => join)
- [_] rpg maker port tool
