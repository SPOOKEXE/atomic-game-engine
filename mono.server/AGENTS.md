# mono.server - module invariants

The server program: a `server`-tier library and a thin main over it. It hosts
worlds, publishes them to clients over QUIC, serves the content those worlds
name, and answers an MCP control surface. `Server.hpp` carries the argument for
each field; this file is what a reviewer should refuse.

## This binary contains no renderer

Not "does not start one". Does not contain one. `mono.server` links no
`client`-tier target, so `render`, `audio`, `input`, `msl` and `resources` are
absent from the link line. That list is the whole of it: the client
program's row in `expected_graph.json` minus the server's, closed over module
links, is those five and nothing else.

**Which is narrower than it sounds, and the narrowness is the design.** This
program does link `effects`, `gui`, `graph`, `script` and `scene`, because all
five are `shared`: a world has particles and a GUI tree and a render graph
whether or not anybody is drawing it, and a server that could not hold them
could not replicate them. The rule is not "no graphics vocabulary"; it is **no
device**. `scene/AGENTS.md` states the test that decides which is which - could
a machine with no graphics stack installed produce this value and mean it?

Three things follow, and all three are checked rather than aspirational:

- The `server` preset configures with no graphics stack: `CMakePresets.json` sets
  `MONO_BUILD_CLIENT: OFF`.
- The staged `server/` directory has no `shaders/` folder and no `client/` one.
  `just check-server-is-headless` is two `test ! -d` lines, and six comments in
  `mono.server/CMakeLists.txt` cite it as the standing proof.
- `mono.tools/architecture/expected_graph.json` records what this program links,
  and `just test-architecture` compares the real graph against it.

If you need something that only exists on the client side, the answer is not to
link it. It is either that the thing belongs in a `shared` module, or that the
server does not actually need it.

## Do not include a client header

`mono.client/include/client/` is invisible here by construction, and the tier
check is what makes it so. `mono.client/CMakeLists.txt` keeps an
`ALLOW_TIER_ESCAPE Mono::server` line commented out and says why it stays that
way: the escape exists for single-player, where the client hosts a server
in-process, and it has not been needed.

**When something genuinely has to be shared it becomes a `shared` engine module,
and that has happened.** `server/Simulation.hpp` used to declare a `Position`, a
`Velocity` and a `WorldBounds` of its own, and the client declared a matching
set to receive them over the wire. All of them are gone: this program links
`Engine::scene`, registers `scene`'s components under `scene`'s names, and a
client registering the same names applies a snapshot from here with no
translation layer at all.

So **a component declared in this directory that means something a `scene`
component already means is the change to refuse.** `server.Chatter` and
`server.Heard` are the two that remain, both in `Simulation.hpp`, and neither is
a duplicate of anything: they exist to put traffic on a bus that would otherwise
carry none, and they go when a game file brings traffic of its own.

**Nothing checks that pair, so by rule 6 this paragraph is the rule.**
`mono.server/tests/ComponentInvariants.cpp` audits every component this program
registers, but for serialisation rules and property rules - not for whether a
name duplicates a `scene` one. A third component here is a reviewer's catch.

## QUIC is the default transport, and one UDP port carries both

`--transport quic|datagram|both`, or `server.transport` in the settings file,
defaulting to **`quic`**. The boolean `--quic` it replaced is gone rather than
kept as an alias: a boolean beside a three-valued flag has no defined answer when
somebody passes both.

**The server decides and the client has no matching flag.** A client opens with
QUIC and falls back on its own, so `datagram` and `both` are operator decisions
that need nothing changed at the other end. The LAN advert carries the mode
(`Announcement.Transports`), so a client on the same network usually skips the
fallback entirely.

**One socket, discriminated on the first byte of the first packet from an unknown
peer.** A QUIC long header sets bit 7 of byte 0; a `net::Packet` opens with a
magic whose first byte has bit 7 clear. `engine::net::WireOf` is that one test
and `mono.engine/net/include/engine/net/Wire.hpp` is where it is argued. Short
header QUIC is never classified at all - it is routed by connection id first, and
only an unroutable packet reaches the discriminator.

**Both refusal directions are explicit, and that is what makes the fallback cost
a round trip rather than a deadline.** `replication::Listener::Refuse` answers a
QUIC opener on a datagram-only server with a Version Negotiation packet, which
needs no keys, and a datagram opener on a QUIC-only server with a `Refusal`
naming the wire to try instead. A server that simply dropped the packet would
make every mismatched client wait out its connect timeout. Neither direction is
mono.server's code: this program chooses the mode and `replication` enforces it.

`docs/QUIC.md` sections 0, 8 and 12 are the current description of the wire.

## The tick is fixed, and the delta is not measured

`Server::Run` computes `1.0 / Settings.TickRate` **once, outside the loop**, and
feeds that constant to every tick. Never the elapsed time. A tick is a function
of its state and its inbox. Feed it real elapsed time and a recorded run stops
replaying, every physics result becomes machine-dependent, and the divergence
shows up somewhere far from the cause.

If a tick overruns, the loop counts it in `RunSummary::Overruns` and carries on.
It does not simulate extra ticks to catch up, and more than four budgets behind
it gives up on the missed ticks entirely rather than trying to make them up: a
server that answers a lost second with thirty back-to-back ticks falls further
behind, and that spiral is much harder to diagnose than a dropped tick.

## The world counts its own ticks

`Server` keeps no tick counter. The loop's `ticks`, the `--ticks` limit and both
summary log lines all read one lambda, `ticksSoFar`, which returns
`Worlds().StatisticsOf(PrimaryWorld).Ticks` - a copy of the world's own clock. A
second tally on the host is a fact that can disagree with itself the first time
one of the two is advanced inside a branch, and the disagreement surfaces as a
summary nobody trusts. `mono.server/tests/Server.cpp` holds it in "the world
keeps its own tick count".

The flag is `--ticks N`, not `--max-ticks`; `Options::MaximumTicks` is the field.

Same reason `scene::WorldBounds` is a resource rather than a component. It was
the same four bytes on every entity - a property of the world stored 4096 times,
which the bounce loop then loaded per row. It lives in `scene` now, because the
client's `SceneBounds` was the same idea under a second name.

## Pacing is against an absolute schedule

`nextTickAt += budget`, not `sleep(budget - spent)`. The second form
accumulates the sleep's own overshoot, so the server drifts slower than its
stated rate and nothing in the numbers says why. Skipped entirely under
`--unpaced` and under a replay, both of which want to run as fast as they can.

## The signal handler does one atomic store

`Stop()` sets an atomic that the loop reads between ticks, and the handler calls
nothing else. Do not grow it. Logging, allocating or touching the world from a
signal handler is undefined behaviour that works right up until it does not.
Installed for `SIGINT` and `SIGTERM`.

## `Components::Seal()` is called at start-up, and determinism rests on it

`app/main.cpp` calls `ecs::Components::Seal()` after `host.Initialise` and before
`host.Run`. That position is the whole point: `Initialise` is what registers
everything, and the seal has to come after the last registration and before the
first tick.

**What it buys is not safety, it is reproducibility.** Registration order fixes
component ids, ids fix archetype iteration order, and iteration order fixes the
order floats are summed in. A component that first reaches `Components::Of<T>`
after this line aborts the process rather than quietly shifting every id after
it, and a script declaring a component late gets `Status::Sealed` from
`Schemas::Register` instead. Decision 14 is strict IEEE, and `just determinism`
is what would otherwise report the drift a long way from its cause.

## Anything not part of the simulation is pumped outside the tick

The MCP control surface, the Discord presence and the content service are all
pumped in `Server::Run` between `FrameGraph::BeginFrame()` and the tick's work,
never inside it. **A recorded run has to reproduce whatever the tick did**, so
whether anybody is connected, fetching a chunk or holding an MCP session open
must not be able to change it. Adding a pump inside the tick is the change to
refuse, and `just determinism` and `just replay-check` are what catch it.

The one apparent exception is the content relay, which is pumped inside
`ServeClients` and **after** `Publish` on purpose: it can only ever spend link
budget the snapshot did not, so a busy world starves downloads rather than the
other way round.

## A replay serves nobody and ages nothing

`ServeClients` and `UpdateWorldLifecycle` are both inside `if (!Replayer_)`. A
network stream and a wall-clock idle suspension would each make a replay depend
on something outside the recording, which is the one thing a replay may not do.

## The store is borrowed for the publish window and null at every other moment

`Server::Publishing` is set for the duration of one `Authority::Publish` and
cleared after it, and `PositionOf` returning `false` outside that window is
contract rather than a bug.

**It is a hoist, and the measurements are why it is shaped this way.**
`Universe::Enter` per candidate put 61% of the whole tick inside the priority
scorer on a 200-client host, and `SurveyVisibility`'s hoist took out an interest
predicate that was walking the world at 41% of the tick. Both numbers are in
`Server.hpp` beside the members. Reaching for `Universe::Enter` inside a
replication hook is the regression to refuse.

## The replication hooks live here because `replication` may not link `scene`

`SetPriority`, `SetPriorityRefinement`, `SetPreface`, `SetOwnership` and
`SetInterest` are all installed from this program. **The rules live in `scene`
and the plumbing lives here**: the wire's job is to move components, and it has
no business knowing what a service is or which part belongs to which player.

One number is shared between two hooks and they must agree about it:
`REPLICATED_FIRST` sits deliberately above `DistancePriority`'s `0..1` range, so
a component the game must have arrives before anything ranked by distance.
Changing it in one hook and not the other is a bug the type system cannot see,
which by rule 6 makes this paragraph the only thing saying so.

## The publish spreads over lanes above eight clients, and the joins never do

`Authority::Publish` runs the steady state over `parallel::Jobs` once there are
`AuthoritySettings::ParallelClientThreshold` (**8**) or more clients, and keeps
the join half on the store-owning thread because `Capture` builds a whole world.
`AuthoritySettings::JoinsPerTick` (**2**) bounds how many join snapshots one
publish builds.

**Both defaults are the library's, and mono.server sets neither.** That is worth
knowing before tuning a deployment: there is no flag, so every host runs the same
two numbers. The measured ladder is in `replication/Authority.hpp` - the join
bound turned "0 of 200 clients could join a 10,000-entity world" into "200 of
200", because a tick that built sixteen joins took 1.8 seconds and `Capture` is
about 113 ms for ten thousand entities in `release`.

The five fields this program does set on the listener are `Audit.Enabled`,
`MaximumClients`, `Wire`, the QUIC TLS seed, and a clamp of `Quic.BytesPerTick`
to the link's own budget.

## The authority resolves attachments in `PostSimulation`

`PrepareSimulation` registers `resolve-attachments` in `Phase::PostSimulation`,
which is where the authority's answer for the tick is finished and where
`World::Step` flushes signals immediately afterwards. A client registers its own
copy in `PreSimulation` instead, and the two are not interchangeable: a server
resolving in `PreSimulation` would publish last tick's transforms.

**It puts bytes on the wire.** `scene.Attachment` replicates whole, so every
attachment costs about 56 bytes per tick it changes. That is the reason to know
this is on rather than to assume it is free.

## The server both serves and publishes content, and the mode says which

`assets::DeclareContentFlags` is called here for **both** verbs, `Handle` and
`Publish`, because a server is the one program that does both. Two content
surfaces follow:

- **Serving**, from `--content-store DIR` with `--content-port` and a 64 hex
  character `--content-grant-key`. `BeginServingContent` also bakes collision
  geometry out of the manifest and merges it into every world already built,
  which is why the order in `Initialise` matters.
- **Fetching**, from `server.content-sources` - a repeatable list whose order is
  priority, spelled exactly as `client.content-sources` is.

`--content-mode relay|redirect` decides what a client is told. **Relay is the
default and it is the safe one**: the server proxies chunks and no endpoint of
its own leaves the process. `Redirect` hands out endpoints and lets clients fetch
directly. `ContentRelay` is the rate limiter that makes relay survivable - a
token bucket per client, a cap on outstanding requests, and a flood cooldown -
and its counters are reported at shutdown, but only when it actually
served something: six numbers on one line, gated on `Requests > 0`. A relay that
refused, a relay that rate-limited and a relay whose budget was full are three
incidents with three different fixes, so one number for them would bury
whichever mattered.

## The server drains and reports `core::Metrics`

Always at shutdown, unconditionally, and on an interval with `--metrics-every N`
(zero, the default, means shutdown only). A report that has to be switched on is
a report nobody switches on.

**`Metrics::Snapshot()` and never `Metrics::Drain()`**, which is the invariant
worth stating: the interval report and the shutdown report would otherwise take
each other's numbers, and the shutdown total would be "everything since the last
interval" while claiming to be the run. `Drain` belongs to the client's overlay,
which has one reader. The read side of `core::Metrics` - counters, gauges and
histograms with nearest-rank percentiles - arrived at v0.19; before that there
was nothing to report.

## The server registers MCP tools of its own

`src/Control.cpp` is `Server::RegisterControlTools`, called from `Run` when
`--mcp-port` (or `server.control-port`) is not `-1`. It adds the shared table
first with `Surface::AddStandardTools` and **then** overrides `engine_info`,
which is the order to keep: the override is written after the thing it replaces,
so a reader sees both.

Three tools are this program's own shape rather than the shared one:
`engine_info` (the game, the tick loop, the listener, the universe and the
control port), `host_link` (the listener's admitted, dropped, turned, rejected,
challenged and refused counts) and `host_players` (one row per client with a
`Player`, and its round trip). The last two fail with an actionable message when
this server was started without `--listen`, rather than reporting zeroes.

## The rewind history records what the world may move

`ServeClients` fills `replication::Rewind` from
`Query<const Transform, const RigidBody>().With<Simulated>()`, skipping
`BodyKind::Static` one layer in. **`scene::Simulated` is the question actually
being asked**; `RigidBody` is only what supplies `Kind`, and `Transform` is the
value recorded.

The predicate is load-bearing and it has been wrong twice.

It walked `Motion` until v0.15, on the reading that a `Motion` is what makes a
placement worth remembering - and `physics` *takes a row's `Motion` away* when it
puts the body to sleep, so that the solver's query never visits a resting row.
The history therefore held whatever happened to be awake. A player standing still
is asleep within a second, which meant they could not be shot, and it presented
as an ordinary miss: a hit test against an empty candidate list strikes nothing
and reports nothing.

It was then the *absence* of `RigidBody` until v0.18, when the tag was `Anchored`
and marked the immovable ones. `scene::Simulated` inverted that polarity, and
**a sleeping body keeps the tag** - it loses `Motion` and nothing else - so the
v0.15 fix survived the change rather than being re-broken by it. An anchored part
never gets `Simulated` at all, so the static geometry the original predicate was
aiming at is still excluded.

## A hit takes health off here, and a client's copy of that number is a copy

`ApplyInputs` recolours what was struck *and* subtracts `SHOT_DAMAGE` when the
struck part belongs to a character. The character is found through the struck
part's **parent**, because the rewind history holds parts and `scene::Character`
sits on the model above. Both results cross as ordinary replicated state -
`scene.Visual` and `scene.Humanoid` - so every client sees this process's verdict
rather than the shooter's, which is the same division the whole function opens
with: a client sends where it aimed and never what it hit.

Two things a reviewer should refuse:

- **A damage figure drawn from a random number.** `SHOT_DAMAGE` is a constant for
  `scene::FindSpawn`'s reason about picking a pad in tree order: a roll inside a
  tick is a recording that does not replay, and `just replay-check` reports it a
  long way from here.
- **A second subtracting path.** `scene::TakeDamage` is the one door, and its
  refusal on `Store::AdoptOnly` is what stops a client running this same code
  against its own replica and deciding who died. Reaching into
  `scene::Humanoid::Health` directly walks past that.

## A client's input tick is a claim, and a stale one means the world went quiet

A client stamps its input with the newest tick it has *applied*, and a tick
reaches it only when something changed. In a still scene its idea of the
server's clock stops advancing while the server's does not, so
`Rewind::TickSeenBy` can name a tick that has fallen out of the ring - and
`Rewind::Each` answers nothing, which is a miss with no error anywhere.

`ApplyInputs` resolves an out-of-window tick **at the present** rather than at
the oldest frame held. That follows from why it goes stale: a world that has not
been changing looks the same now as it did then. It also cannot be gamed -
rewinding is the favourable answer for a laggy shooter, so claiming a tick this
server no longer remembers buys the least favourable resolution there is, not
the most. A tick inside the window is honoured exactly as before, and
`RewindSettings::HistoryTicks` (32) remains the fairness bound.

An empty history is a separate case and is skipped rather than resolved: there is
no present to fall back to.

## A host never listens, and a listening driver holds exactly one world

`--host` and `--remote-world` together is refused in `main`. A listening server
sets `WorldsPerHost` to `1` whatever `--worlds-per-host` said, so one replication
authority owns one UDP endpoint and one failure boundary. Children are spawned
with `--listen 0` so each asks the operating system for its own port rather than
inheriting a number that is already taken.

## Two limits can turn a player away, and they are not the same limit

`Options::MaximumClients` is what the transport admits; `Players.MaxPlayers` is
what the world lets in. A client can pass the first and fail the second, and the
`ENGINE_WARN` on that path exists because a silent return leaves somebody
connected to a world they can never enter, with nothing anywhere saying why.

## Shutdown order is load-bearing

Recorder, link, discovery, then `Replication.reset()` before `Socket->Close()`
before `Driver_.reset()` before `Jobs::Stop()`. Each step hands back something
the next one is still allowed to touch. `Server::~Server` is out of line for an
unrelated reason worth not undoing: it keeps the CDN types incomplete in the
public header.

## Not here yet

`orchestration` at L12 - sessions, matchmaking, sharding, drain - is a `server`
module and does not exist. Neither do `ledger` nor `persistence`.

Two things this section used to list have since arrived, and neither grew inside
`mono.server`, which is the point:

- **`net` exists** at `mono.engine/net` and this program links it directly.
  Transport, packets, reliability, handshake, congestion, the wire
  discriminator, QUIC and HTTP.
- **A game file format exists**, as `Engine::game` rather than a module named
  `gamefile`. `Server::HostGameFile` opens a `.agame`, and the extension is what
  picks it over a script path.

This directory holds the program's own attachments: the main, the tick loop,
world placement, the drain path, the replication hooks, the content relay, the
Discord presence and the MCP control surface. Anything reusable belongs under
`mono.engine/`.
