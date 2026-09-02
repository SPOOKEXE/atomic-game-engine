#pragma once

// Headless server-owned tick, world set and shutdown path.

#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/datastore/Backend.hpp>
#include <engine/datastore/Http.hpp>
#include <engine/datastore/Provider.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Shooting.hpp>
#include <engine/game/Play.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/replication/Rewind.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/DataStore.hpp>
#include <engine/world/Driver.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Lifecycle.hpp>
#include <engine/world/Recording.hpp>
#include <engine/world/Universe.hpp>

#include <cstdint>
#include <discord/Link.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <network/Advert.hpp>
#include <network/Presence.hpp>
#include <optional>
#include <string>
#include <vector>

// The content attachment is held by pointer and named nowhere else in this
// header - see `Server::~Server`.
namespace cdn {
	class Origin;
	class Service;
}

namespace engine::assets {
	class GrantKey;
}

namespace engine::scene {
	struct CollisionShapes;
}

namespace engine::game {
	class OpenedProject;
}

namespace server {
	class ContentRelay;
	struct ContentRelayStatistics;
}

namespace server {

	// Which way this server gives its clients content.
	//
	// **A field rather than two server classes**, which is `cdn::CDNSettings`'
	// rule one layer up: the moment the mode is a *type*, moving between them is
	// a rebuild rather than a configuration change, and which way a server
	// serves content has to stay a deployment decision.
	//
	// @since v0.16
	enum class ContentMode : uint8_t {
		// This server holds the origin connection and streams content to its
		// clients over the game link they already have.
		//
		// **The default**, because it is the deployment that asks least of a
		// player's network: one port, already reached, already admitted. The
		// client has no authority in it - it may ask and it may ask again, and
		// `ContentRelay` decides how often.
		Relay,

		// This server tells clients where the origins are and they fetch for
		// themselves, presenting the grant it issued them.
		Redirect,
	};

	// Returns a stable, human-readable name for a content mode.
	//
	// @param mode The mode to name.
	// @return A view valid for the lifetime of the process.
	// @since v0.16
	const char *Describe(ContentMode mode);

	// Parses what `server.content-mode` was set to.
	//
	// @param text The setting's value.
	// @return The mode, or nothing when the text names neither.
	// @since v0.16
	std::optional<ContentMode> ContentModeOf(std::string_view text);

	// Command-line configuration copied into Server during Initialise.
	struct Options {
		// The loopback port the control surface listens on, or -1 for off.
		//
		// Unauthenticated world control; disabled by default.
		int ControlPort = -1;

		// Ticks per second. A world ticks at its own rate; this is the rate the
		// one world this version hosts runs at.
		double TickRate = 30.0;

		// Physics steps per second, and snapshots published per second, for the
		// worlds this program creates itself. Zero follows `TickRate`.
		//
		// **Only for the worlds this program creates.** A `.agame` carries a
		// rate per world and those win, for the reason `TickRate` does not
		// overwrite them either: a scene authored to solve at 30 should solve
		// at 30 whoever hosts it. `world::WorldSettings` documents what the two
		// numbers mean.
		//@{
		double PhysicsTickRate = 0.0;
		double ReplicationTickRate = 0.0;
		//@}

		// Entities in the placeholder world, until there is a game file to load
		// one from.
		uint32_t Entities = 4096;

		// -1 runs until stopped. A tick budget is what makes the server usable
		// from a test or a CI job.
		int64_t MaximumTicks = -1;

		// Run for this long, then drain and exit. Zero means run until stopped.
		double Seconds = 0.0;

		// Run every tick back to back instead of pacing to TickRate. For
		// measuring the simulation rather than the sleep between ticks.
		bool Unpaced = false;

		// Whether to manage world lifetime at all.
		//
		// **Off by default, and that is the whole reason this is a setting.**
		// `docs/DEFERRED.md` D00017 hoisted the idle policy into
		// `world::DecideLifecycle` so an editor and a server could not disagree
		// about when a world closes, and then deliberately left this program
		// without a caller: suspension is a behaviour change to the one program
		// whose output `just determinism` and `just replay-check` compare byte
		// for byte. Off by default makes those comparisons unaffected by
		// construction rather than by the runs happening to be short.
		//
		// The policy is not repeated here. What a host supplies is what only it
		// can know - whether a world is occupied - and for a server that is a
		// player standing in it.
		//
		// @since v0.13
		bool ManageWorldLifetime = false;

		// When an empty world stops ticking, once management is on.
		//
		// **`Never` is a real answer rather than a way of switching this off.**
		// A world can be empty of players and full of things that have to keep
		// happening - NPCs on a route, an economy, a countdown between rounds.
		// `world::IdleSleep` says why that is an enum instead of a very large
		// number.
		//
		// @since v0.13
		engine::world::IdleSleep IdleSleepMode = engine::world::IdleSleep::Timeout;

		// How long a world may sit empty under `IdleSleep::Timeout`.
		//
		// Clamped by the decision to `world::MAXIMUM_IDLE_LIMIT_SECONDS`.
		//
		// @since v0.13
		double IdleCloseSeconds = engine::world::DEFAULT_IDLE_LIMIT_SECONDS;

		// The scene to host. Empty means the placeholder world.
		//
		// Scene script loaded with the same path as the client.
		std::string GamePath;

		// Read assets from here instead of from beside the binary. Empty means
		// the default location. Set once during Initialise, before anything has
		// resolved a path through it.
		std::filesystem::path AssetsDirectory;

		// Root holding isolated mock and live files in the selected backend.
		// Empty keeps the process-local store used before persistence existed.
		std::filesystem::path DataStoreRoot;

		// The adapter assigned to the default logical datastore.
		engine::datastore::Provider DataStoreProvider = engine::datastore::Provider::File;

		// The local provider's durable file format. Ignored by HTTP.
		engine::datastore::Backend DataStoreBackend = engine::datastore::Backend::Binary;

		// Whether an operator or embedding API chose any DataStore field. A
		// universe manifest fills the defaults only when this is false.
		bool DataStoreConfigured = false;

		// Mock and live never share a file, even under one configured root.
		engine::world::SharedStoreEnvironment DataStoreEnvironment =
			engine::world::SharedStoreEnvironment::Live;

		// Remote provider connection. Used only when DataStoreProvider is HTTP.
		engine::datastore::HttpDataStoreSettings HttpDataStore;

		// Write a recording of the run here. Empty means record nothing.
		//
		// A recording is one snapshot plus every envelope applied since, which
		// is complete because a world is deterministic given its state and its
		// inbox. Reading it back reproduces the run exactly - same binary, same
		// machine.
		std::filesystem::path RecordPath;

		// Replay this recording instead of simulating.
		//
		// The world's own outbox is discarded for each replayed barrier, since
		// a replayed world re-derives the same requests and applying both
		// copies would double every operation.
		std::filesystem::path ReplayPath;

		// Write a folded-stack profile of the whole run here. Empty writes none.
		//
		// **A whole run rather than a window, which is what distinguishes it
		// from `--graph`.** `FrameGraph`'s retained history is five seconds deep
		// and keeps one worst reading per span name; a folded capture keeps the
		// call tree and accumulates it for as long as the server runs, which is
		// what a flamegraph of a load test needs. `scripts/flamegraph.py` turns
		// the file into an SVG.
		//
		// Turning it on turns frame collection on with it - a profile of a run
		// nobody collected is an empty file - so a run that names one is a run
		// paying the collector's cost. That is a measurement, not a deployment.
		//
		// @since v0.16
		std::filesystem::path ProfilePath;

		// Ticks between windowed snapshots of `ProfilePath`, each written beside it
		// as `<stem>.window<NNNNN><extension>`. Zero writes none.
		//
		// **A cumulative writer sampled periodically, not a second collector.**
		// `FrameGraph::WriteFolded` reads the running total without clearing it, so
		// two snapshots N ticks apart subtract into exactly that window's folded
		// stacks - `scripts/flamegraph.py --average` does the subtracting. Nothing
		// here has to know that; it only has to sample on a schedule.
		//
		// Ignored when `ProfilePath` is empty, for the same reason `ProfilePath`
		// alone turns collection on: a window of a profile nobody asked for is
		// still a cost with nothing measuring it.
		//
		// @since v0.18
		uint64_t ProfileWindowTicks = 0;

		// Seconds between periodic `core::Metrics` reports. Zero writes none.
		//
		// **The headless server counted things for ten versions and read none
		// of them.** `cdn`, `assets`, `delivery`, `net` and `replication` all
		// write into `core::Metrics`; the only reader in the tree was the
		// client's F5 overlay, which a server does not have. So every counter a
		// host produced went into a sink and was thrown away at exit -
		// `docs/ARCH_REVIEW.md` §G2.
		//
		// **Off by default, and that is deliberate rather than timid.** A report
		// on a schedule is output nobody asked for, and a host's log is scraped:
		// `just stress` reads the tick line out of it. A run that wants the
		// numbers says so, and gets them on the interval it named.
		//
		// The final report at shutdown is **not** gated by this. It costs one
		// snapshot at the end of a run that is already over, and a counter
		// nobody can see is the whole finding.
		//
		// @since v0.19
		double MetricsReportSeconds = 0.0;

		// Write a heap report here when the run ends. Empty writes none.
		//
		// Turning it on turns heap sampling on, for the reason `ProfilePath`
		// turns frame collection on: a run given the flag and nothing else would
		// retain no readings and then report that it had nothing to fit a slope
		// to.
		//
		// **The server is the process this matters most on.** A client is
		// restarted when somebody closes it; a host runs for days, so a leak
		// that is a rounding error over a session is the thing that ends the
		// process over a weekend.
		//
		// @since v0.18
		std::filesystem::path HeapReport;

		// Host the world in a universe that permits several. Named worlds are
		// the unit a supervisor grants and revokes. Above one, a headless server
		// automatically spreads the worlds across physical-core-bound processes.
		uint32_t Worlds = 1;

		// Run as a supervised host under a driver, rather than as a driver.
		//
		// Empty means driver; set means this process is a supervised host.
		std::string HostName;

		// The worlds this host was granted, by name.
		//
		// Names rather than ids: an id is an index into one process's registry
		// and means something else here. Empty with `HostName` set is a host
		// that was given nothing, which is an error rather than an idle
		// process - a supervisor that granted nothing has a bug in its plan.
		std::vector<std::string> HostWorlds;

		// Worlds to place into supervised host processes rather than hold here.
		//
		// The driver still routes every bus operation, so a world's behaviour
		// does not change when it moves to another address space. Automatic
		// multi-world placement fills this list before the driver starts.
		std::vector<std::string> RemoteWorlds;

		// How many `Shared` worlds may sit in one host.
		uint32_t WorldsPerHost = 8;

		// The program a host runs. Empty means this executable.
		std::filesystem::path HostProgram;

		// How many processes share this machine, including this one.
		//
		// Zero lets the driver size one shared worker budget for all processes.
		uint32_t Processes = 0;

		// Physical-core slot assigned by the driver. UINT32_MAX means no binding.
		// Host children receive this through their private launch protocol.
		uint32_t PhysicalCore = UINT32_MAX;

		// Whether to serve the primary world to clients at all.
		//
		// Off by default, and that is what every existing recipe and test gets:
		// a server that opened a socket because nobody said not to would be a
		// behaviour change in the determinism run, and a port bound by accident
		// is a port somebody else cannot bind.
		//
		// **A host never listens.** A host holds worlds for a driver and the
		// driver is the authority for all of them; two processes each streaming
		// the same world to the same client is the split brain the federated
		// universe exists to prevent.
		bool Listening = false;

		// The UDP port to serve on. Only read when `Listening`.
		//
		// Zero binds an ephemeral port, which is a real answer rather than a
		// way of saying no - a test wants one so that two runs on one machine do
		// not collide. `Server::ListeningOn` says which was chosen. That is why
		// there is a flag beside it rather than zero meaning off.
		uint16_t ListenPort = 0;

		// The hard cap on connected clients, once `Listening`.
		//
		// **A deployment number rather than a library one.**
		// `replication::ListenerSettings::MaximumClients` defaults to 64 and is
		// the bound an unadmitted flood can fill, which `replication/AGENTS.md`
		// is explicit about; what a host can actually carry is a property of the
		// machine and of the world, and until v0.16 no flag said so. A load test
		// with two hundred clients on it had nothing to raise.
		//
		// Raising it costs what the module says it costs: the per-client link
		// budget times this, out of one uplink.
		//
		// @since v0.16
		uint32_t MaximumClients = 64;

		// Make every world this process builds talk on a bus.
		//
		// There is no game yet, so there is no traffic. This is the only thing
		// that crosses a driver-to-host link until a game file arrives with
		// traffic of its own, and it is what makes a multi-process universe
		// demonstrable rather than merely startable. Off by default; passed
		// down to hosts when it is on.
		bool Chatter = false;

		// A content store this server serves to its own clients.
		//
		// Empty disables the attached origin.
		std::filesystem::path ContentStore;

		// The port the attached origin listens on. Only read when
		// `ContentStore` is set. Zero binds an ephemeral one.
		uint16_t ContentPort = 0;

		// Numeric address clients may use to reach the attached origin.
		//
		// Empty keeps the wildcard listening address private. Redirect mode needs
		// an explicit address because `0.0.0.0` is not a client destination.
		std::string ContentPublicHost;

		// The secret shared with whoever issues grants - which, for an attached
		// origin, is this same process.
		//
		// Empty disables grants and therefore cannot serve an attached origin.
		std::string ContentGrantKey;

		// Which way clients are given content.
		//
		// @since v0.16
		ContentMode ContentDelivery = ContentMode::Relay;

		// Whether an operator explicitly selected the mode. False lets a package
		// supply a weaker public hint after config and environment precedence.
		bool ContentDeliveryConfigured = false;

		// Whether public HTTP sources declared by a project may be used.
		// A package never turns this on by itself.
		bool AllowPackageHttp = false;

		// The origins this server fetches content from, in priority order.
		//
		// **Spelled exactly as the client's `--cdn` is** - `dir:PATH` for a
		// published tree and `HOST:PORT` for an origin, optionally `NAME=` in
		// front - because they mean the same thing and a second spelling is a
		// second thing to get wrong. The order *is* the policy, which is
		// `delivery/AGENTS.md`'s rule: there is no strategy flag beside the list.
		//
		// In `Relay` these are where relayed routes come from. In `Redirect` they
		// are what a client is told about. Empty falls back to `ContentStore`,
		// so a self-hosted server needs one flag rather than two saying the same
		// thing.
		//
		// @since v0.16
		std::vector<std::string> ContentSources;

		// The publisher whose signature content must carry, as 64 hex characters.
		//
		// Handed to clients in `Redirect` so they have a root of trust, and used
		// in `Relay` to check a relayed manifest before it is passed on - which is
		// defence in depth rather than a boundary, because the client checks it
		// again regardless.
		//
		// @since v0.16
		std::string ContentPublisherKey;

		// Whether to announce this server on the local subnet.
		//
		// Off by default, and only meaningful with `Listening`: a server that
		// announced a port it never bound would be advertising nothing. The
		// default is off for `Listening`'s reason - a process that broadcast
		// because nobody said not to would be a behaviour change in every
		// existing recipe.
		//
		// @since v0.13
		bool Advertise = false;

		// What to call this session in somebody's browser. Empty uses the game
		// file's name, or a placeholder when there is none.
		//
		// @since v0.13
		std::string SessionName;

		// The pre-shared secret that makes this session private, or empty for a
		// public one.
		//
		// Taken as 64 hex characters when it is exactly that, and as a
		// passphrase otherwise. Both are the same key on the other end, and a
		// person who was given words types words.
		//
		// @since v0.13
		std::string SessionSecret;

		// A rendezvous point to register with, as `host:port`, or empty.
		//
		// What makes this server reachable by somebody who is not on the same
		// subnet. Independent of `Advertise`: a dedicated server on the
		// internet registers and announces to nobody, and a LAN game announces
		// and registers nowhere.
		//
		// @since v0.13
		std::string RendezvousAddress;

		// Which transports this server answers. `quic` by default.
		//
		// **The server decides and the client has no flag.** A client opens with
		// QUIC and falls back when it is refused, so `datagram` and `both` are
		// operator decisions that need nothing changed at the other end. The
		// boolean `--quic` this replaces is gone rather than kept as an alias:
		// nothing in the tree depended on it, and a boolean beside a three-valued
		// flag has an undefined answer when somebody passes both.
		//
		// A QUIC server with no `IdentityKey` draws an ephemeral one rather than
		// refusing to start - `replication::ListenerSettings::Quic` says why, and
		// what it gives is exactly what an unsigned datagram welcome gives.
		//
		// @since v0.19
		engine::net::WireMode Transport = engine::net::WireMode::Quic;

		// The Ed25519 seed this server proves its identity with, as 64 hex
		// characters, or empty for none.
		//
		// **Without it the exchange authenticates nobody**, which is protection
		// against a listener and not against a relay - see
		// `Listener::SetIdentity`. The same key a publisher signs manifests
		// with, so a deployment distributes one public key and not two.
		std::string IdentityKey;

		// Client public keys admitted to this server. Empty leaves admission open.
		// The corresponding seeds stay with the platform and clients.
		std::vector<std::string> AdmittedKeys;
	};

	// Balanced placement for duplicated headless worlds.
	struct WorldProcessPlan {
		// Total processes, including the driver.
		uint32_t Processes = 1;

		// Worlds retained in the driver's process.
		uint32_t LocalWorlds = 1;

		// Child host processes to spawn.
		uint32_t RemoteHosts = 0;
	};

	// Chooses at most one process per assignable physical core, leaving one
	// core unused by default for operating-system and background work.
	//
	// @param worlds        Total duplicated worlds.
	// @param physicalCores Assignable physical cores, or zero when unavailable.
	// @param requested     Explicit process count, or zero for automatic.
	// @return A process count and the driver's first balanced partition.
	// @since v0.20
	WorldProcessPlan PlanWorldProcesses(uint32_t worlds, uint32_t physicalCores, uint32_t requested = 0);

	// What the run produced. Returned rather than logged only, so a test can
	// assert on it.
	struct RunSummary {
		// Ticks simulated, read from the world's own clock rather than counted
		// here - there is only ever one tally, so there is nothing to disagree.
		uint64_t Ticks = 0;

		// Wall time the run took, in seconds. Includes the pacing sleep, so
		// against a paced run this is roughly Ticks / TickRate and says nothing
		// about how hard the ticks were.
		double Seconds = 0.0;

		// The worst single tick, in milliseconds.
		//
		// The number to read first. A mean that looks healthy hides a tick that
		// blew its budget once, and it is the once that a player feels.
		float SlowestTickMilliseconds = 0.0f;

		// Mean time spent inside a tick, in milliseconds, over the ticks that
		// ran. Time spent waiting for the next one is not in it.
		float MeanTickMilliseconds = 0.0f;
		// Ticks that overran their budget. The number that says whether the
		// tick rate was actually held.
		uint64_t Overruns = 0;

		// The tick cost distribution, in milliseconds.
		//
		// **A mean and a maximum are two numbers and the shape is a third
		// thing.** A load test's whole question is whether a server holds its
		// rate with N clients on it, and a mean says yes while a fiftieth of the
		// ticks miss the budget - which is what a player feels. Nearest-rank
		// over every tick of the run, so p99 is a tick that actually happened.
		//
		// @since v0.16
		//@{
		float TickP50Milliseconds = 0.0f;
		float TickP95Milliseconds = 0.0f;
		float TickP99Milliseconds = 0.0f;
		//@}
	};

	// One hosted world and the fixed-tick loop that drives it.
	//
	// Initialise, then Run until it returns, then Shutdown. Run blocks for the
	// life of the world, so anything that needs to end it - a signal handler, a
	// test - does so through Stop from another thread.
	class Server {
	  public:
		Server();

		// Applies every pending input, server-authoritatively.
		void ApplyInputs();

		// Writes one client's move onto the humanoid it is allowed to move.
		//
		// **The lookup is the whole of the security here.** A client names
		// nothing - it says only which way it is trying to walk - so the body
		// the intent lands on is the one this server assigned to that
		// connection, and a client that sent a hundred moves still moves one
		// character.
		//
		// @param client Who sent it.
		// @param move   What they asked for, already normalised by the decoder.
		void ApplyMove(engine::replication::ClientId client, const engine::game::MoveInput &move);

		// Where an entity is, for the priority score and its occlusion query.
		//
		// **Only answers during a publish**, because that is the only thing that
		// asks and because the answer comes out of `Publishing` - see that
		// member. Outside one it reports `false`, which the scorer already
		// handles: an entity with no position falls back on the rotation.
		bool PositionOf(engine::ecs::Entity entity, engine::core::Vector3 &out) const;

		// Where a client is looking from, or `false` when nothing placed it.
		bool ViewpointOf(engine::replication::ClientId client, engine::core::Vector3 &out) const;

		// Works out what every client can see, before any of them is served.
		//
		// Fills `HiddenFromClients` and `OwnedByPlayer` - see those members for
		// why the client-independent half of the interest predicate is hoisted
		// out of the per-client loop.
		//
		// **Not `const`, because `EachEntity` is not**: walking a store binds it
		// to the calling thread, which is a write however read-only the body is.
		//
		// @param store The world about to be published.
		void SurveyVisibility(engine::ecs::Store &store);

		// Where moving things were, for judging a client's action against what
		// that client actually saw.
		//
		// **The query is a game's to make and the answer is a game's to act
		// on.** What the server owes is an accurate record and
		// `Rewind::TickSeenBy` to turn a client's input tick and its link's
		// round trip into the moment to sample.
		//
		// **Nothing calls this accessor today**, and the claim it used to carry
		// - that nothing consumes the history at all - stopped being true when
		// `ApplyInputs` gained the shot path, which asks over `History`
		// directly. What is unwired is the *outward* half: a game asking its own
		// question of the same record. `docs/CODE_ARCH.md` decision 16.
		//
		// @return The history.
		const engine::replication::Rewind &Rewound() const {
			return History;
		}

		// Tells the authority where a client is looking from, for the priority
		// score.
		//
		// **A host's job rather than the authority's**, because a client's
		// viewpoint is a game's idea and this engine has no per-client avatar
		// yet: the placeholder world is cubes and nobody is in it. A client
		// nothing has placed scores everything the same, which is the round
		// robin the score replaces and the honest answer rather than pretending
		// every client stands at the origin.
		//
		// @param client The client.
		// @param at     Where it is looking from, in world space.
		void SetClientViewpoint(engine::replication::ClientId client, const engine::core::Vector3 &at);

		// Forgets where a client was looking.
		//
		// @param client The client.
		void ForgetClientViewpoint(engine::replication::ClientId client);

		// Points every client's viewpoint at its own character.
		//
		// **The caller `SetClientViewpoint` waited three versions for.** That
		// hook and `DistancePriority` have both existed since v0.4, and the
		// reason nothing filled them in was written into the comment beside
		// them: there was no per-client avatar, so the placeholder world was
		// cubes and nobody was in it. There is one now - `scene::AddPlayer` per
		// connection, with a character - so the honest answer stopped being
		// "score everything the same".
		//
		// Called once a tick from `ServeClients`, before the delta is built.
		// A client between a join and a spawn is *forgotten* rather than left
		// at its last position, because a round robin is a better answer than a
		// stale one.
		//
		// @since v0.15
		void UpdateClientViewpoints();

		// Declared so the content attachment can stay an incomplete type in
		// this header. `mono.server` links `Mono::cdn`, and a server's public
		// header pulling the origin's in behind it would put a content-delivery
		// dependency on every translation unit that includes this.
		~Server();

		Server(const Server &) = delete;
		Server &operator=(const Server &) = delete;

		// Applies `options`, starts the job system and builds the world.
		//
		// @param options Parsed command line. Copied, not referenced.
		// @return False if the options do not describe a runnable server - a
		//         tick rate of zero or less is the case that exists today. The
		//         caller exits; nothing has been started yet at that point.
		bool Initialise(const Options &options);

		// Tears down the world and stops the job system. Safe after a failed
		// Initialise, so an exit path does not have to know how far it got.
		void Shutdown();

		// Runs the fixed-tick loop until the budget is spent or Stop is called.
		RunSummary Run();

		// Where the attached origin is listening, or nothing.
		//
		// Worth asking for even when the port was chosen: zero binds an
		// ephemeral one, and this is how a test and a launcher learn which.
		//
		// @return The port, or nothing when no content is being served.
		std::optional<uint16_t> ContentPort() const;

		// What the content relay has done, or nothing when there is none.
		//
		// @return The counters. A caller reads them to tell a client asking too
		//         fast from an upstream that is down, which are two incidents with
		//         two different fixes.
		// @since v0.16
		const ContentRelayStatistics *ContentRelayStats() const;

		// Issues a grant admitting every bundle this server serves.
		//
		// **This is the server's half of the grant and the only half it has.**
		// The server decides what a client may have; the origin checks a token
		// and serves. A client is handed this and presents it, and the origin
		// learns nothing about who asked.
		//
		// Today it grants the whole publication, because there is no interest
		// set to narrow it by - no player, no position, no entitlement. That is
		// the honest scope for what exists rather than a security decision, and
		// it is the line to change when a session knows what it needs.
		//
		// @param session Which session this belongs to.
		// @param nowSeconds The current time.
		// @param lifetimeSeconds How long it is good for.
		// @return The token, or nothing when no content is being served.
		std::optional<std::vector<std::byte>>
		IssueContentGrant(uint64_t session, uint64_t nowSeconds, uint64_t lifetimeSeconds = 300) const;

		// Safe from another thread: the loop reads it between ticks.
		void Stop();

		// Runs `body` against the primary world's storage.
		//
		// **Scoped, because the universe hands out identifiers rather than
		// stores.** A long-lived `Store &` is what makes thread-per-world and
		// process-per-world different designs; a reference that exists for the
		// length of a call does not. The day this world lives in a child
		// process, this becomes a request and the callers do not change.
		//
		// @param body Called as `body(Store &)` on the driver thread.
		// @return `false` when there is no world to enter.
		bool Enter(const std::function<void(engine::ecs::Store &)> &body);

		// The universe this server drives.
		//
		// For a caller that needs more than the primary world - a supervisor,
		// or a test that creates a second one.
		//
		// @return The universe.
		engine::world::Universe &Worlds() {
			return Driver_->Worlds();
		}

		// The universe and the hosts holding the rest of it.
		//
		// @return The driver, or null before Initialise.
		engine::world::Driver *Hosts() {
			return Driver_.get();
		}

		// The primary world's handle.
		//
		// @return The handle, or an invalid one before Initialise.
		engine::world::WorldId Primary() const {
			return PrimaryWorld;
		}

		// Whether this process is a supervised host.
		//
		// @return `true` when it holds worlds for a driver.
		bool IsHost() const {
			return !Settings.HostName.empty();
		}

		// The replication endpoint, when this server is listening.
		//
		// For a caller that wants to declare what is replicated or read how many
		// clients are connected - and for a test, which is the only way to prove
		// the socket was actually bound.
		//
		// @return The listener, or null when `--listen` was not given.
		engine::replication::Listener *Clients() {
			return Replication.get();
		}

		// The address the replication socket is bound to.
		//
		// Worth asking for even though the port was named: `--listen 0` binds an
		// ephemeral port, which is what a test wants so that two runs on one
		// machine do not collide.
		//
		// @return The endpoint, or an invalid one when not listening.
		engine::net::Endpoint ListeningOn() const;

	  private:
		// Builds one world, from a scene file when `--game` names one and from
		// the placeholder otherwise.
		//
		// One place rather than the two call sites that used to build the
		// placeholder directly: a locally hosted world and a remotely hosted
		// one authoring different scenes is a divergence nothing would report,
		// because each side is internally consistent.
		//
		// @param store     The world to build into.
		// @param scheduler The systems to install.
		// @return `false` when a named scene could not be loaded.
		bool BuildWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler);

		// Expands `Worlds` into one balanced local partition and supervised
		// remote partitions before the job pool or any world exists.
		bool ConfigureWorldPlacement();

		// Opens a game, universe folder, or Project ZIP and starts its worlds.
		//
		// @return `false` when the file would not load or holds no worlds.
		bool HostProject();

		// Loads the configured driver-owned DataStore before any world ticks.
		bool LoadDataStore();

		// Writes only when the driver-owned records changed since the last flush.
		bool FlushDataStore();

		// Builds the worlds a host was granted and announces itself.
		//
		// @return `false` when there is no link, or no worlds to hold.
		bool InitialiseHost();

		// Opens the discovery presence, once the listening port is known.
		//
		// **After the socket, and it has to be**: `--listen 0` binds an
		// ephemeral port, and an announcement carrying the port that was asked
		// for rather than the one that was bound sends every client somewhere
		// nothing is listening.
		//
		// @return `false` only when a secret was given and is not one. A
		//         subnet that will not carry a broadcast is a fault recorded on
		//         the presence, not a refusal to serve.
		bool BeginAnnouncing();

		// Announces, and tells the world how full this server is.
		//
		// @param nowSeconds The current time.
		void ServeDiscovery(double nowSeconds);

		// How many hosts `RemoteWorlds` will be placed into.
		//
		// Run before anything is spawned, because the worker budget depends on
		// it and the job pool starts first.
		//
		// @return The host count, zero when nothing is remote.
		size_t PlannedHosts() const;

		// Places the worlds `RemoteWorlds` names into hosts and starts them.
		//
		// @return `false` when a host could not be started.
		bool StartHosts();

		// Exchanges one barrier's worth of frames with the driver.
		//
		// Called once per tick, before the tick: what the driver decided last
		// barrier has to be in the inboxes before the systems that read them
		// run, and what this host's worlds posted goes up straight after.
		//
		// @return `false` when the driver asked this host to stop, or went
		//         away.
		bool ServiceLink();

		// Starts the recorder when `RecordPath` is set.
		//
		// Shared by the fresh-world and the replay path, because a replay that
		// accepted `--record` and wrote nothing is how the recipe that checks
		// the replay path came to check nothing.
		//
		// @return `false` when recording was asked for and cannot be done.
		bool BeginRecording();

		// Starts the attached origin when `ContentStore` is set.
		//
		// @return `false` when a store was asked for and cannot be served.
		bool BeginServingContent();

		// Gives one world the collision geometry its colliders name.
		//
		// The built-in shapes always, because a `MeshPart` set to `Cube` needs
		// one before any content exists, and `ContentShapes` when the store has
		// been read. Called beside `PrepareSimulation`, so every world this
		// process simulates gets it and the placeholder benchmark world - which
		// has no physics at all - does not.
		void InstallCollisionShapes(engine::ecs::Store &store);

		// Builds the relay that answers clients' content routes.
		//
		// @return `false` when relay mode was asked for and could not be set up,
		//         which is a configuration failure rather than a missing feature:
		//         a server told to relay and quietly relaying nothing is a client
		//         staring at a world with no textures and nothing saying why.
		bool BeginRelayingContent();

		// The content sources this server was configured with.
		//
		// Parsed from `Options::ContentSources`, falling back to `ContentStore`.
		//
		// @return The sources, in priority order.
		std::vector<engine::delivery::Source> ConfiguredContentSources() const;

		// Tells one client where content is, in `ContentMode::Redirect`.
		//
		// @param client Who to tell.
		void OfferContentDirectory(engine::replication::ClientId client);

		// Binds the replication socket and declares what is replicated.
		//
		// @return `false` when a port was asked for and could not be bound.
		bool BeginListening();

		// Runs one tick's worth of replication: take what arrived, publish what
		// changed, advance the links.
		//
		// Called from inside the world's scope, because the delta is built from
		// the store's change bits and those are the world's.
		//
		// @param nowSeconds The current time.
		void ServeClients(double nowSeconds);

		// Suspends worlds nobody is in, and wakes ones something arrived for.
		//
		// **The decision is `world::DecideLifecycle` and none of it is repeated
		// here.** This gathers what only a server can know and applies what came
		// back - D00017's whole argument is that a host growing its own idle
		// policy makes a world that closes in the editor and not on the server,
		// with nothing reporting the difference.
		//
		// What a server supplies that an editor does not: occupancy is a player
		// standing in the world, where the studio also counts the active scene
		// and a viewport looking at it. Nothing here has a viewport.
		//
		// Does nothing at all when `Options::IdleCloseSeconds` is zero, which is
		// the default - see there for why that matters to two recipes.
		//
		// @param nowSeconds The current time.
		void UpdateWorldLifecycle(double nowSeconds);

		// When each active world was last occupied.
		//
		// **Only active worlds have an entry, and that is the ordering bug
		// D00017 records from the studio's own move.** Looking the clock up for a
		// suspended world creates an entry it has no use for and delays waking it
		// by a frame - so the lookup happens inside the `Active` branch.
		struct WorldLife {
			engine::world::WorldId World;
			double LastOccupied = 0.0;

			// Whether a `scene::AwakeWorld` claim was holding it up last pass,
			// so the log line is written when that starts rather than every
			// tick it stays true.
			bool HeldAwake = false;
		};
		std::vector<WorldLife> Lives;

		// Whether a claim was holding this world up on the previous pass.
		bool WasHeldAwake(engine::world::WorldId world) const;

		// Records whether a claim is holding this world up.
		void SetHeldAwake(engine::world::WorldId world, bool held);

		// Restarts one world's idle clock, adding an entry if it has none.
		//
		// @param world The world that was just occupied.
		// @param nowSeconds The current time.
		void TouchWorld(engine::world::WorldId world, double nowSeconds);

		// Adds this server's own tools after the engine feature list.
		//
		// **In `src/Control.cpp`, beside the state it reads**, which is the same
		// place the editor keeps its own. Called by the custom feature in `Run`,
		// and only when `--mcp-port` asked for a surface at all.
		//
		// @since v0.19
		void RegisterControlTools();

		// The control surface. Started only when asked; a server that was never
		// started costs a thread that was never spawned.
		engine::control::Server ControlServer;
		engine::control::Surface ControlSurface{
			"atomic-server",
			"A dedicated server of the atomic game engine, hosting worlds headlessly. `world_list` "
			"is worth calling first: a world is a scene and the universe is the game. This program "
			"authors nothing - it hosts, so there is no selection and no run mode to change."
		};

		Options Settings;

		// Owns temporary package extraction until every world and content user is
		// destroyed. Declared before the driver so reverse destruction is safe.
		std::unique_ptr<engine::game::OpenedProject> HostedProject;

		// Public project content considered after operator policy is applied.
		std::filesystem::path ProjectContentStore;
		std::vector<engine::delivery::Source> ProjectContentSources;
		std::string ProjectPublisherKey;

		// The universe this process holds, plus the hosts holding the rest.
		//
		// Always a driver, even with no hosts: a `Driver` with an empty
		// supervisor is a `Universe` and five lines of nothing, and one code
		// path that sometimes has hosts beats two that differ in where the
		// barrier lives.
		//
		// Held by pointer so the header does not have to be complete before the
		// options are read - a universe binds its driver thread on
		// construction, and that thread is decided in Initialise.
		std::unique_ptr<engine::world::Driver> Driver_;
		engine::world::WorldId PrimaryWorld;

		// Placement derived once from `Options::Worlds`. The driver keeps the
		// first partition and each supervised host receives one of the rest.
		uint32_t LocalWorlds = 1;
		uint32_t RemoteHosts = 0;
		bool AutomaticWorldPlacement = false;

		// One route and one snapshot for the universe. Worlds reach the shared
		// store through `Driver`, so no per-world persistence copy can diverge.
		std::unique_ptr<engine::world::DataStoreRouter> DataStorePersistence;
		std::vector<engine::world::SharedStoreEntry> PersistedDataStore;
		bool DataStoreReady = false;

		// How this server is found - the LAN beacon and the rendezvous
		// registration. Null when `--advertise` and `--rendezvous` were both
		// absent, which is every existing recipe.
		//
		// Held by pointer for the reason the driver is: the header stays free
		// of what it takes to build one.
		//
		// @since v0.13
		std::unique_ptr<network::Presence> Discovery;

		// What is being announced, kept so the player count can be refreshed
		// without rebuilding the record every tick.
		network::Advert Announcement;

		// This tick's time, for the foreign-datagram handler. The listener's
		// drain has no clock of its own to pass on, and reading one inside
		// would put a wall clock in the middle of the tick - which is the thing
		// `net/AGENTS.md` bans.
		double DiscoveryNow = 0.0;

		// The clock the last `ServeClients` was called with.
		//
		// **Because an admission callback has no time of its own.** `Listener::
		// OnAdmitted` fires from inside `Poll`, and the join notice it sends
		// needs the same `nowSeconds` every other send on this link takes -
		// reading a clock there would be a second time source in a program whose
		// whole tick is passed in.
		double PollNow = 0.0;

		// The content origin this process serves, when one was asked for.
		//
		// Held by pointer for the reason the driver is: the header stays free
		// of what it takes to build one. Declared in this order because the
		// service borrows the origin and must be destroyed first.
		std::unique_ptr<cdn::Origin> ContentOrigin;
		std::unique_ptr<cdn::Service> ContentService;

		// The relay that answers clients' route requests, in `ContentMode::Relay`.
		//
		// Null in `Redirect`, and null in `Relay` when no content source was
		// configured - a server hosting a game whose content ships inside its game
		// file relays nothing and should build nothing.
		std::unique_ptr<ContentRelay> ContentLink;

		// The issuing half of the shared secret.
		//
		// **Two holders of one key, and that is the design rather than a
		// duplication.** The grant splits the job in two: the server decides
		// what a client may have and issues a token, the origin checks it and
		// serves. Here both ends happen to be this process, and the key still
		// exists twice because the *roles* do - collapsing them into one object
		// would make the in-process arrangement a different code path from the
		// deployed one, which is exactly what the transport refuses too:
		// single-player runs a loopback carrying real encoding.
		std::unique_ptr<engine::assets::GrantKey> ContentGrantSecret;

		// The collision geometry of every mesh in the content store.
		//
		// **Because a headless server has to agree with a client about what a
		// mesh collides as.** A `Collider` naming a hull resolves through
		// `scene::CollisionShapes`, and until v0.17 the only thing that ever
		// filled that table was the client, on the frame an asset arrived. A
		// server therefore had mesh colliders it could not resolve and every one
		// of them silently fell back to the part's bound - which reads as a
		// client and a server disagreeing about where a player is standing.
		//
		// **Baked once and merged into each world**, rather than per world: the
		// hull is quickhull over the model and the table is copied whole by
		// `SetResource`, so a host of eight worlds would otherwise read its
		// store eight times. `game::AddCollisionShapesFrom` fills this;
		// `InstallCollisionShapes` puts it on a world.
		//
		// Held by pointer for the reason the origin is - the header stays free
		// of the geometry types. Null until `BeginServingContent` runs, and null
		// for ever on a server with no `--content-store`, which then has the
		// built-in shapes and nothing else.
		//
		// @since v0.17
		// arch-waiver ecs-copy: baked once for the host and merged into each world
		// by `InstallCollisionShapes`. The paragraph above is the argument: a host
		// of eight worlds would otherwise run quickhull eight times over the same
		// meshes to produce eight identical tables.
		std::unique_ptr<engine::scene::CollisionShapes> ContentShapes;

		// One VM per world, while a game file is being hosted.
		//
		// **Held here as well as by each world's scheduler**, because the
		// scheduler's copy is a capture inside a lambda and nothing else names
		// it. A server that dropped its own would still work and would be one
		// refactor away from not, which is the kind of lifetime nobody wants to
		// re-derive.
		std::vector<std::shared_ptr<engine::script::Runtime>> Runtimes;

		std::unique_ptr<engine::world::Recorder> Recorder_;
		std::unique_ptr<engine::world::Replayer> Replayer_;

		// The replication socket and the clients on it. Both null unless
		// `--listen` was given, which is what keeps a headless determinism run
		// from binding a port it has no use for.
		std::unique_ptr<engine::net::Transport> Socket;
		std::unique_ptr<engine::replication::Listener> Replication;

		// What Discord is told this server is hosting, or null when nothing is
		// configured. Off unless `discord.enabled` and `discord.app-id` are
		// both set, which is every default install.
		//
		// @since v0.17
		std::unique_ptr<discord::Link> DiscordLink;

		// When this process started, as a unix epoch second, for the elapsed
		// timer. Read once, because `discord::Link` takes monotonic seconds and
		// Discord wants epoch ones.
		//
		// @since v0.17
		int64_t DiscordStartedUnixSeconds = 0;

		// Makes the link, if the flags asked for one. Called once, from `Run`.
		//
		// @since v0.17
		void StartDiscord();

		// Says what is being hosted, and keeps the connection alive.
		//
		// **Beside the control surface rather than in the tick.** A presence
		// update is not part of the simulation and changes nothing a recorded
		// run has to reproduce, which is the same argument `ContentService` is
		// pumped here on.
		//
		// @param nowSeconds This process's monotonic clock.
		// @since v0.17
		void PumpDiscord(double nowSeconds);

		// What the Discord templates can name, filled from this tick.
		//
		// @return The tokens and what they resolve to.
		// @since v0.17
		discord::Facts DiscordFacts();

		// The identity `Replication` signs transcripts with, held here because
		// `Listener::SetIdentity` borrows rather than copies: a `SigningKey` is
		// move-only and zeroes itself, and a copy would be a second place a
		// secret lives.
		std::optional<engine::assets::SigningKey> Identity;

		// The platform admission set. Restriction remains active after the final
		// key is revoked so an empty live whitelist denies everybody.
		std::vector<engine::assets::PublicKey> AdmittedClientKeys;
		bool AdmissionRestricted = false;

		// Where moving things were, for the last `RewindSettings::HistoryTicks`
		// ticks.
		//
		// **Recorded whether or not anything asks**, because the alternative is
		// a game turning it on and finding the history empty for the first half
		// second. It costs one map insert per moving entity per tick and no
		// allocation after the ring has filled.
		engine::replication::Rewind History;

		// Reused across ticks so resolving a shot allocates nothing after the
		// first one, which is the same reason every other per-tick buffer in
		// this engine is a member rather than a local.
		std::vector<engine::examples::Target> Candidates;

		// How many inputs were resolved and how many were not this encoding.
		//
		// **Two counters and not one.** A shot that hit nothing is an ordinary
		// event; a payload that did not decode is a client running a different
		// game or probing, and burying the second in the first is how an
		// unusual number stops being noticed.
		uint64_t Struck = 0;
		uint64_t Dropped = 0;

		// Where one client is looking from.
		struct Viewpoint {
			engine::core::Vector3 At;

			// **Kept beside the position and checked on every read.** A slot is
			// reused when a client leaves and another joins, so an entry keyed
			// on the index alone would hand the new client the old one's
			// viewpoint - and sort its world by where somebody else stood.
			uint32_t Generation = 0;
		};

		// Which `Player` instance one client is.
		struct Occupant {
			engine::ecs::Entity Instance;

			// Checked on every read, for the reason `Viewpoint` carries one: a
			// slot is reused the moment a client leaves, and an entry keyed on
			// the index alone would hand the new client the old one's player -
			// and with it everything that player owned.
			uint32_t Generation = 0;
		};

		// Who is in this game, keyed by client slot.
		//
		// **The map ownership is decided through.** A client's delta is checked
		// against `scene::NetworkOwner` on the entity and the player found here;
		// a client with no entry owns nothing, which is where every connection
		// starts.
		std::unordered_map<uint32_t, Occupant> Players;

		// Where each client is looking from, for `DistancePriority`.
		//
		// **Keyed by the slot rather than by the whole handle**, so the map is
		// bounded by the client limit rather than growing once per connection
		// for the life of the process. A client absent from here has no
		// viewpoint, which the score reads as "order these by rotation alone".
		std::unordered_map<uint32_t, Viewpoint> Viewpoints;

		// What `SetInterest` needs that does not depend on who is asking, worked
		// out once per publish.
		//
		// **Because two of the predicate's three questions are about the world
		// and only the third is about the client.** Whether an entity is inside
		// a server-scoped service, and which `Player` it sits under, are facts
		// about the tree - and the predicate is asked once per entity *per
		// client*, so on a two-hundred-client host each of those tree walks ran
		// two hundred times for one answer. A capture put 41% of the tick in the
		// interest walk.
		//
		// **Derived and thrown away inside one publish, which is what keeps it
		// out of rule 2's way.** Nothing here is a second copy of a fact the ECS
		// owns for longer than the call that reads it: `SurveyVisibility` fills
		// it from the store immediately before `Publish` walks the same store,
		// and nothing between the two mutates the world. It cannot drift,
		// because it does not survive long enough to.
		//
		// Both are sorted by entity id, and both are usually tiny - the entities
		// under a player, and the entities inside `ServerScriptService` and
		// `ServerStorage`. Everything else answers "visible and unowned" after
		// two failed binary searches.
		//@{
		std::vector<uint64_t> HiddenFromClients;
		std::vector<std::pair<uint64_t, engine::ecs::Entity>> OwnedByPlayer;
		//@}

		// The store `Listener::Publish` is walking, for as long as it is walking
		// it. Null at every other moment.
		//
		// **A borrow rather than a copy, which is the distinction rule 2
		// draws.** Nothing is duplicated and nothing outlives the call: this is
		// the same `ecs::Store` the surrounding `Worlds().Enter` already handed
		// out, kept where the priority callbacks can reach it.
		//
		// **It is here because `Universe::Enter` per candidate is what a
		// two-hundred-client server was spending itself on.** The scorer runs
		// once per candidate per client - which at 200 clients over a two
		// thousand entity world is around a million calls a tick - and each of
		// `PositionOf` and the `ReplicatedFirst` test opened the world again to
		// read one component out of the store the publish was already inside.
		// A capture of that arrangement put 61% of the whole tick in the scorer;
		// `.cache/stress/RESULTS.md` carries the numbers either side.
		engine::ecs::Store *Publishing = nullptr;

		// Present only in host mode. A driver holds one of these per host; a
		// host holds exactly one, to whoever started it.
		std::unique_ptr<engine::world::HostLink> Link;

		// Reused across barriers so a host servicing its link every tick stops
		// allocating.
		std::vector<engine::world::HostFrame> Frames;

		bool Running = false;

		// There is no tick counter here. The world keeps one - its own clock -
		// and a second copy on the host is a fact that can disagree with itself
		// the first time one of them is advanced in a branch.
	};
}
