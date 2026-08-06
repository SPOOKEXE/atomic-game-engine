#pragma once

// The server program's own code — an attachment on top of the engine, not a
// layer of it.
//
// It owns the fixed tick, the world set and the shutdown path. It contains no
// renderer: `mono.server` links no `client`-tier target, so `render`, `ui`,
// `audio`, `input`, `text` and `vfx` are absent from the link line entirely. A
// server-side script that reaches for input will fail because the class was
// never registered into this binary, not because a check rejected it.
//
// There is no window, no swapchain and no SDL here. That is checkable rather
// than aspirational: the `server` preset configures with no graphics stack and
// the staged `server/` directory has no `shaders/` folder.

#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Shooting.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/replication/Rewind.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Driver.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Recording.hpp>
#include <engine/world/Universe.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The content attachment is held by pointer and named nowhere else in this
// header — see `Server::~Server`.
namespace cdn {
	class Origin;
	class Service;
}

namespace engine::assets {
	class GrantKey;
}

namespace server {

	// Everything the command line decides, in one place.
	//
	// Parsed once and copied into the Server by Initialise, so nothing reads the
	// argument list again after start-up and there is one answer to "what is
	// this process configured to do".
	struct Options {
		// The loopback port the control surface listens on, or -1 for off.
		//
		// **Off by default, and that is the security boundary.** The surface
		// reads and writes world state for whatever connects to it, with no
		// authentication — see `SECURITY.md`. `--mcp-port` is the opting in.
		int ControlPort = -1;

		// Ticks per second. A world ticks at its own rate; this is the rate the
		// one world this version hosts runs at.
		double TickRate = 30.0;

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

		// The scene to host. Empty means the placeholder world.
		//
		// **A scene script, not yet a game file.** Since v0.3 this runs through
		// the same loader the client's `--script` does, over the same file, so
		// both ends author the world identically — a server that built it its
		// own way would disagree with its replicas once a tick and every side
		// would look self-consistent while doing it.
		//
		// The flag keeps the name it will have when `mono.gamefile` lands and a
		// universe of worlds is what this points at, rather than being renamed
		// twice. Until then a game is one scene.
		std::string GamePath;

		// Read assets from here instead of from beside the binary. Empty means
		// the default location. Set once during Initialise, before anything has
		// resolved a path through it.
		std::filesystem::path AssetsDirectory;

		// Write a recording of the run here. Empty means record nothing.
		//
		// A recording is one snapshot plus every envelope applied since, which
		// is complete because a world is deterministic given its state and its
		// inbox. Reading it back reproduces the run exactly — same binary, same
		// machine.
		std::filesystem::path RecordPath;

		// Replay this recording instead of simulating.
		//
		// The world's own outbox is discarded for each replayed barrier, since
		// a replayed world re-derives the same requests and applying both
		// copies would double every operation.
		std::filesystem::path ReplayPath;

		// Host the world in a universe that permits several. Named worlds are
		// the unit a supervisor grants and revokes; one is simply the common
		// case rather than the only one.
		uint32_t Worlds = 1;

		// Run as a supervised host under a driver, rather than as a driver.
		//
		// **A host is not a different program.** It is this one, holding some
		// of a universe's worlds in its own address space so that a hard fault
		// in one of them takes this process rather than the server. That is
		// what makes the grouping a deployment decision instead of an engine
		// one, and it is the reason there is no `mono.host`.
		//
		// Empty means this process is a driver. Set, it names this host to its
		// driver — and the process then expects a channel it was started with,
		// and refuses to start without one.
		std::string HostName;

		// The worlds this host was granted, by name.
		//
		// Names rather than ids: an id is an index into one process's registry
		// and means something else here. Empty with `HostName` set is a host
		// that was given nothing, which is an error rather than an idle
		// process — a supervisor that granted nothing has a bug in its plan.
		std::vector<std::string> HostWorlds;

		// Worlds to place into supervised host processes rather than hold here.
		//
		// **Processes are for crash isolation, not for speed.** A world here
		// costs a process and an address space and buys exactly one thing:
		// a hard fault in it takes that process rather than the server. Two
		// worlds in two processes are not faster than two in two threads.
		//
		// The driver still routes every bus operation, so a world's behaviour
		// does not change by being moved out — which is what makes this a
		// deployment decision.
		std::vector<std::string> RemoteWorlds;

		// How many `Shared` worlds may sit in one host.
		uint32_t WorldsPerHost = 8;

		// The program a host runs. Empty means this executable.
		std::filesystem::path HostProgram;

		// How many processes share this machine, including this one.
		//
		// **Every process calling `Jobs::Start(0)` is the bug this prevents.**
		// That asks for one worker per hardware thread, so a driver and seven
		// hosts on a twenty-four core machine run a hundred and ninety threads
		// over twenty-four cores, and every one of them is slower than it would
		// have been alone.
		//
		// Zero means work it out: a driver counts the hosts it is about to
		// spawn and tells each of them the total, so the arithmetic is done
		// once by the only process that knows the answer.
		uint32_t Processes = 0;

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
		// way of saying no — a test wants one so that two runs on one machine do
		// not collide. `Server::ListeningOn` says which was chosen. That is why
		// there is a flag beside it rather than zero meaning off.
		uint16_t ListenPort = 0;

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
		// **CDN.md §6's "local store", and it is an attachment rather than a
		// second program.** `mono.cdn` is the same library deployed on its own
		// and scaled; running one in-process is the self-hosted case — a small
		// game, a LAN session, development — and the client cannot tell the
		// difference, which is the property §11 asks for. One store, one
		// manifest format, three deployments.
		//
		// Empty serves no content, which is what every existing recipe gets:
		// a server that bound a port because nobody said not to would be a
		// behaviour change in the determinism run, for `Listening`'s reason.
		std::filesystem::path ContentStore;

		// The port the attached origin listens on. Only read when
		// `ContentStore` is set. Zero binds an ephemeral one.
		uint16_t ContentPort = 0;

		// The secret shared with whoever issues grants — which, for an attached
		// origin, is this same process.
		//
		// **Still checked, and still a real MAC over a real token.** CDN.md §4
		// is explicit that single-player and LAN use the same flow with the
		// server in-process: the grant is issued and verified across a function
		// call and both halves are real. A path skipped in the configuration
		// people develop against is a path that breaks the first time somebody
		// ships the other one — §16.6's argument, applied to content.
		//
		// Empty means the attached origin generates one at start-up and keeps
		// it to itself, which is right when this process is the only issuer.
		std::string ContentGrantKey;

		// The Ed25519 seed this server proves its identity with, as 64 hex
		// characters, or empty for none.
		//
		// **Without it the exchange authenticates nobody**, which is protection
		// against a listener and not against a relay — see
		// `Listener::SetIdentity`. The same key a publisher signs manifests
		// with, so a deployment distributes one public key and not two.
		std::string IdentityKey;
	};

	// What the run produced. Returned rather than logged only, so a test can
	// assert on it.
	struct RunSummary {
		// Ticks simulated, read from the world's own clock rather than counted
		// here — there is only ever one tally, so there is nothing to disagree.
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
	};

	// One hosted world and the fixed-tick loop that drives it.
	//
	// Initialise, then Run until it returns, then Shutdown. Run blocks for the
	// life of the world, so anything that needs to end it — a signal handler, a
	// test — does so through Stop from another thread.
	class Server {
	  public:
		Server();

		// Applies every pending input, server-authoritatively.
		void ApplyInputs();

		// Where an entity is, for the priority score and its occlusion query.
		// Not `const`: it enters a world, which takes it.
		bool PositionOf(engine::ecs::Entity entity, engine::core::Vector3 &out);

		// Where a client is looking from, or `false` when nothing placed it.
		bool ViewpointOf(engine::replication::ClientId client, engine::core::Vector3 &out) const;

		// Where moving things were, for judging a client's action against what
		// that client actually saw.
		//
		// **The query is a game's to make and the answer is a game's to act
		// on.** This engine has no notion of a shot, so nothing here consumes
		// it; what the server owes is an accurate record and
		// `Rewind::TickSeenBy` to turn a client's input tick and its link's
		// round trip into the moment to sample.
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
		// @return False if the options do not describe a runnable server — a
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

		// Issues a grant admitting every bundle this server serves.
		//
		// **This is the server's half of CDN.md §4 and the only half it has.**
		// The server decides what a client may have; the origin checks a token
		// and serves. A client is handed this and presents it, and the origin
		// learns nothing about who asked.
		//
		// Today it grants the whole publication, because there is no interest
		// set to narrow it by — no player, no position, no entitlement. That is
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
		// For a caller that needs more than the primary world — a supervisor,
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
		// clients are connected — and for a test, which is the only way to prove
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

		// Whether `--game` names a game file rather than a scene script.
		//
		// By extension. `--game` has accepted a `.luau` since v0.3 and every
		// recipe that uses it still passes one, so the new format is added
		// beside the old rather than in place of it.
		//
		// @param path What `--game` was given.
		// @return `true` for a `.agame`.
		static bool IsGameFile(const std::string &path);

		// Loads a game file's universe and starts every world's scripts.
		//
		// @return `false` when the file would not load or holds no worlds.
		bool HostGameFile();

		// Builds the worlds a host was granted and announces itself.
		//
		// @return `false` when there is no link, or no worlds to hold.
		bool InitialiseHost();

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

		// The control surface. Started only when asked; a server that was never
		// started costs a thread that was never spawned.
		engine::control::Server ControlServer;
		engine::control::Surface ControlSurface{
			"atomic-server",
			"A dedicated server of the atomic game engine, hosting worlds headlessly. `world_list` "
			"is worth calling first: a world is a scene and the universe is the game. This program "
			"authors nothing — it hosts, so there is no selection and no run mode to change."
		};

		Options Settings;

		// The universe this process holds, plus the hosts holding the rest.
		//
		// Always a driver, even with no hosts: a `Driver` with an empty
		// supervisor is a `Universe` and five lines of nothing, and one code
		// path that sometimes has hosts beats two that differ in where the
		// barrier lives.
		//
		// Held by pointer so the header does not have to be complete before the
		// options are read — a universe binds its driver thread on
		// construction, and that thread is decided in Initialise.
		std::unique_ptr<engine::world::Driver> Driver_;
		engine::world::WorldId PrimaryWorld;

		// The content origin this process serves, when one was asked for.
		//
		// Held by pointer for the reason the driver is: the header stays free
		// of what it takes to build one. Declared in this order because the
		// service borrows the origin and must be destroyed first.
		std::unique_ptr<cdn::Origin> ContentOrigin;
		std::unique_ptr<cdn::Service> ContentService;

		// The issuing half of the shared secret.
		//
		// **Two holders of one key, and that is the design rather than a
		// duplication.** CDN.md §4 splits the job in two: the server decides
		// what a client may have and issues a token, the origin checks it and
		// serves. Here both ends happen to be this process, and the key still
		// exists twice because the *roles* do — collapsing them into one object
		// would make the in-process arrangement a different code path from the
		// deployed one, which is exactly what §16.6 forbids for the transport.
		std::unique_ptr<engine::assets::GrantKey> ContentGrantSecret;

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

		// The identity `Replication` signs transcripts with, held here because
		// `Listener::SetIdentity` borrows rather than copies: a `SigningKey` is
		// move-only and zeroes itself, and a copy would be a second place a
		// secret lives.
		std::optional<engine::assets::SigningKey> Identity;

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
			// viewpoint — and sort its world by where somebody else stood.
			uint32_t Generation = 0;
		};

		// Where each client is looking from, for `DistancePriority`.
		//
		// **Keyed by the slot rather than by the whole handle**, so the map is
		// bounded by the client limit rather than growing once per connection
		// for the life of the process. A client absent from here has no
		// viewpoint, which the score reads as "order these by rotation alone".
		std::unordered_map<uint32_t, Viewpoint> Viewpoints;

		// Present only in host mode. A driver holds one of these per host; a
		// host holds exactly one, to whoever started it.
		std::unique_ptr<engine::world::HostLink> Link;

		// Reused across barriers so a host servicing its link every tick stops
		// allocating.
		std::vector<engine::world::HostFrame> Frames;

		bool Running = false;

		// There is no tick counter here. The world keeps one — its own clock —
		// and a second copy on the host is a fact that can disagree with itself
		// the first time one of them is advanced in a branch.
	};
}
