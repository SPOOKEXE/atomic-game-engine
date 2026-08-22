// Thin argument-parsing entry point over the server library.

#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/world/Lifecycle.hpp>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <discord/Settings.hpp>
#include <server/Server.hpp>
#include <server/Settings.hpp>

namespace {
	server::Server *Running = nullptr;

	// Ctrl-C has to reach the tick loop rather than killing the process where
	// it stands. Only Stop is called from here - it is one atomic store, which
	// is about the limit of what is safe in a signal handler.
	extern "C" void OnInterrupt(int) {
		if (Running) {
			Running->Stop();
		}
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("server");

	// Declared before anything is parsed or read - see the client's `main` for
	// why the order matters.
	engine::core::Config::DeclareEngineFlags();
	engine::parallel::DeclareFlags();

	// **Both verbs, because a server is the one program that does both.** It
	// fetches nothing itself, but `--content-store` attaches an origin that
	// publishes, and `Authority` hands content names to clients that decode.
	engine::assets::DeclareContentFlags(engine::assets::ContentVerb::Handle);
	engine::assets::DeclareContentFlags(engine::assets::ContentVerb::Publish);
	server::DeclareFlags();

	// The `discord.*` table, shared with the client and the origin. Declared
	// rather than owned: the wording differs per program and lives in
	// `DiscordPresence.cpp`, but the switches are one table so a config file
	// spells them the same everywhere.
	discord::DeclareFlags();

	engine::core::Arguments arguments("server", "atomic - hosts a game.");
	engine::core::Config::DeclareOptions(arguments);

	arguments.Flag("unpaced", "Tick back to back instead of pacing to the tick rate");
	arguments.Flag("graph", "Collect the frame graph (for a Tracy capture)");
	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag(
		"force-serial-compute",
		"Run every parallel dispatch on one thread, so the frame graph keeps every span"
	);

	// The control surface. Off unless asked for - see `Options::ControlPort`.
	arguments.Value("mcp-port", "PORT", "Listen for Model Context Protocol on 127.0.0.1:PORT (default 8734)");
	arguments.Flag("chatter", "Make every world publish on a shared topic (no game file yet)");

	arguments.Value("tick-rate", "HZ", "Ticks per second (default 30)");
	arguments.Value("physics-tick-rate", "HZ", "Physics steps per second (default: the tick rate)");
	arguments.Value("replication-tick-rate", "HZ", "Snapshots per second (default: every tick)");
	arguments.Value("entities", "N", "Entities in the placeholder world (default 4096)");
	arguments.Value("ticks", "N", "Exit after N ticks");
	arguments.Value("seconds", "N", "Exit after N seconds");
	arguments.Value(
		"idle-close",
		"N|never|immediate",
		"Manage world lifetime: suspend after N seconds, never (24/7), or as soon as empty"
	);
	arguments.Value("game", "PATH", "Game file to host (v0.5+)");
	arguments.Value("record", "PATH", "Write a recording of this run");
	arguments.Value("replay", "PATH", "Replay a recording instead of simulating");
	arguments.Value("override-assets-directory", "DIR", "Read staged data from here");
	arguments.Value("host", "NAME", "Run as a supervised host under a driver, with this name");
	arguments.Value("world", "NAME", "A world this host was granted (repeatable, host mode only)");
	arguments.Value("remote-world", "NAME", "Place this world in a supervised host process (repeatable)");
	arguments.Value("worlds-per-host", "N", "Shared worlds per host process (default 8)");
	arguments.Value("host-program", "PATH", "The program a host runs (default: this one)");
	arguments.Value("processes", "N", "How many processes share this machine (default: worked out)");
	arguments.Value("profile-out", "PATH", "Fold this run's frame graph into a .folded flamegraph capture");
	arguments.Value(
		"heap-report", "PATH", "Write a heap profile when the run ends, and sample while running"
	);
	arguments.Value(
		"profile-window",
		"TICKS",
		"With --profile-out, also snapshot every TICKS ticks for scripts/flamegraph.py --average"
	);
	arguments.Value("listen", "PORT", "Serve the world to clients on this UDP port (0 for ephemeral)");
	arguments.Value("max-clients", "N", "The hard cap on connected clients (default 64)");
	arguments.Flag("advertise", "Announce this server on the local subnet so clients can find it");
	arguments.Value("session-name", "NAME", "What to call this session in somebody's browser");
	arguments.Value(
		"session-key",
		"SECRET",
		"Make this session private: 64 hex characters, or a passphrase. Only clients holding it may join"
	);
	arguments.Value(
		"rendezvous", "HOST:PORT", "Register with a rendezvous point, so clients off this subnet can reach it"
	);
	arguments.Value("content-store", "DIR", "Serve this content store to clients - CDN.md §6's local store");
	arguments.Value("content-port", "PORT", "Port the attached origin listens on (0 for ephemeral)");
	arguments.Value(
		"content-grant-key", "HEX", "64 hex characters - the secret grants are issued and checked with"
	);
	arguments.Flag(
		"quic",
		"Serve over QUIC rather than the datagram wire. Needs --identity-key, and every client must "
		"be started with --quic too"
	);
	arguments.Value(
		"identity-key",
		"HEX",
		"64 hex characters - the Ed25519 seed this server proves its identity with. Without it a "
		"relay in the path can read everything"
	);

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.VersionRequested) {
		std::fputs(arguments.VersionLine().c_str(), stdout);
		return 0;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
		return 0;
	}

	// **`--verbose` before the settings, so it reaches the settings' own
	// complaints**, and kept beside `engine.log-level` because every recipe in
	// this repository passes it.
	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
	}
	if (arguments.Has("force-serial-compute")) {
		engine::core::Flags::Set("engine.serial-compute", "true", engine::core::FlagSource::CommandLine);
	}

	const engine::core::ConfigReport settings = engine::core::Config::Apply(arguments);
	if (!settings.Ok) {
		std::fprintf(stderr, "%s\n", settings.Error.c_str());
		return 2;
	}
	if (engine::core::Config::ListingWanted(arguments)) {
		std::fputs(engine::core::Flags::Listing().c_str(), stdout);
		return 0;
	}

	// Set before startup so every dispatch uses the measured serial path.
	engine::parallel::ApplyFlags();

	// The settings first, the command line over the top - see the client's
	// `main` for the precedence this expresses.
	server::Options options = server::OptionsFromFlags();
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.PhysicsTickRate = arguments.GetNumber("physics-tick-rate", options.PhysicsTickRate);
	options.ReplicationTickRate = arguments.GetNumber("replication-tick-rate", options.ReplicationTickRate);
	options.Entities = static_cast<uint32_t>(arguments.GetInteger("entities", options.Entities));
	options.MaximumTicks = arguments.GetInteger("ticks", -1);
	options.Seconds = arguments.GetNumber("seconds", 0.0);
	// **Absent means unmanaged, which is not the same as `never`.** Unmanaged is
	// this program's behaviour before v0.13 and the one `just determinism` and
	// `just replay-check` compare against; `never` is a deliberate 24/7 server
	// that still resumes a suspended world when something arrives for it.
	if (const std::optional<std::string_view> idleFlag = arguments.Get("idle-close"); idleFlag) {
		const std::string_view idle = *idleFlag;
		options.ManageWorldLifetime = true;
		if (idle == "never") {
			options.IdleSleepMode = engine::world::IdleSleep::Never;
		} else if (idle == "immediate") {
			options.IdleSleepMode = engine::world::IdleSleep::Immediate;
		} else {
			options.IdleSleepMode = engine::world::IdleSleep::Timeout;
			options.IdleCloseSeconds = arguments.GetNumber("idle-close", options.IdleCloseSeconds);

			// Said out loud rather than clamped in silence. The decision clamps
			// anyway - see `world::MAXIMUM_IDLE_LIMIT_SECONDS` - and a number
			// quietly ignored reads as the flag not working.
			if (options.IdleCloseSeconds > engine::world::MAXIMUM_IDLE_LIMIT_SECONDS) {
				ENGINE_WARN(
					"--idle-close {:.4g}s is above the {:.4g}s ceiling and will be capped; "
					"use --idle-close never for a server that should not sleep",
					options.IdleCloseSeconds,
					engine::world::MAXIMUM_IDLE_LIMIT_SECONDS
				);
			}
		}
	}
	// **`||` rather than `=` for every bare flag.** An absent `--unpaced` is
	// silence and not "off", so assigning would let a command line with no such
	// flag on it overrule a config file that asked for one.
	options.Unpaced = options.Unpaced || arguments.Has("unpaced");

	// **`Has` then `GetInteger`, and the two-step is the opt-in.** A bare
	// `GetInteger` with a fallback would open the port on every run, because a
	// fallback is returned when the flag is absent. This way `--mcp-port` alone
	// takes this program's number, and no flag at all leaves whatever
	// `server.control-port` said - which is minus one unless a deployment
	// deliberately opened it.
	if (arguments.Has("mcp-port")) {
		options.ControlPort = static_cast<int>(arguments.GetInteger("mcp-port", 8734));
	}
	options.Chatter = options.Chatter || arguments.Has("chatter");

	if (auto store = arguments.Get("content-store")) {
		options.ContentStore = std::filesystem::path(*store);
	}
	options.ContentPort = static_cast<uint16_t>(arguments.GetInteger("content-port", options.ContentPort));
	if (auto key = arguments.Get("content-grant-key")) {
		options.ContentGrantKey = std::string(*key);
	}
	if (auto key = arguments.Get("identity-key")) {
		options.IdentityKey = std::string(*key);
	}
	options.Quic = options.Quic || arguments.Has("quic");

	if (auto game = arguments.Get("game")) {
		options.GamePath = std::string(*game);
	}
	if (auto record = arguments.Get("record")) {
		options.RecordPath = std::filesystem::path(*record);
	}
	if (auto replay = arguments.Get("replay")) {
		options.ReplayPath = std::filesystem::path(*replay);
	}
	if (auto profile = arguments.Get("profile-out")) {
		options.ProfilePath = std::filesystem::path(*profile);
	}
	if (auto report = arguments.Get("heap-report")) {
		options.HeapReport = std::filesystem::path(*report);
	}
	options.ProfileWindowTicks =
		static_cast<uint64_t>(std::max<int64_t>(0, arguments.GetInteger("profile-window", 0)));
	if (auto assets = arguments.Get("override-assets-directory")) {
		options.AssetsDirectory = std::filesystem::path(*assets);
	}
	if (auto host = arguments.Get("host")) {
		options.HostName = std::string(*host);
		for (const std::string_view world : arguments.GetAll("world")) {
			options.HostWorlds.emplace_back(world);
		}
	} else if (arguments.Has("world")) {
		std::fprintf(stderr, "--world names a world for a host, and needs --host.\n");
		return 2;
	}

	for (const std::string_view world : arguments.GetAll("remote-world")) {
		options.RemoteWorlds.emplace_back(world);
	}
	options.WorldsPerHost =
		static_cast<uint32_t>(arguments.GetInteger("worlds-per-host", options.WorldsPerHost));
	if (auto program = arguments.Get("host-program")) {
		options.HostProgram = std::filesystem::path(*program);
	}
	options.Processes = static_cast<uint32_t>(arguments.GetInteger("processes", options.Processes));

	if (arguments.Has("listen")) {
		const int64_t port = arguments.GetInteger("listen", 0);
		if (port < 0 || port > 65535) {
			std::fprintf(stderr, "--listen takes a port between 0 and 65535.\n");
			return 2;
		}

		// Zero is a real answer here - bind an ephemeral port - so "was it
		// given" and "what was it set to" are different questions and the flag
		// being present is what turns listening on.
		options.ListenPort = static_cast<uint16_t>(port);
		options.Listening = true;
	}

	options.MaximumClients =
		static_cast<uint32_t>(arguments.GetInteger("max-clients", options.MaximumClients));

	options.Advertise = options.Advertise || arguments.Has("advertise");
	if (auto name = arguments.Get("session-name")) {
		options.SessionName = std::string(*name);
	}
	if (auto secret = arguments.Get("session-key")) {
		options.SessionSecret = std::string(*secret);
	}
	if (auto point = arguments.Get("rendezvous")) {
		options.RendezvousAddress = std::string(*point);
	}

	if (!options.HostName.empty() && !options.RemoteWorlds.empty()) {
		// A host that spawned hosts of its own would build a tree nobody
		// planned and nothing supervises above the first level.
		std::fprintf(stderr, "--host and --remote-world are for opposite ends of a link.\n");
		return 2;
	}

	engine::core::FrameGraph::SetEnabled(arguments.Has("graph"));

	server::Server host;
	if (!host.Initialise(options)) {
		// Initialise may have started the job pool before failing.
		host.Shutdown();
		ENGINE_ERROR("server failed to start");
		return 1;
	}

	// **The component table closes here, and this is what makes the determinism
	// promise real rather than intended.** Registration order fixes component
	// ids, ids identify archetypes, and archetypes are iterated in id order -
	// so two runs that register the same types in a different order visit rows
	// in a different order, and a floating-point sum over those rows diverges.
	// `Components::Seal` is what pins it, and until v0.19 its only caller was a
	// test, which meant the guarantee `just determinism` and `just replay-check`
	// rest on was not switched on in any shipped binary.
	//
	// **After `Initialise`, because that is what registers everything.** Every
	// module's `Register*Components` runs during start-up, and a `Store`'s
	// constructor registers the instance components on the way past. Sealing
	// before that would close an empty table.
	//
	// **A script that declares a component after this gets a clean refusal, not
	// a crash.** `Schemas::Register` checks `Components::Sealed()` and returns
	// `Status::Sealed`, whose own comment is the reason above. That path was
	// built for this and had nothing switching it on.
	engine::ecs::Components::Seal();

	Running = &host;
	std::signal(SIGINT, OnInterrupt);
	std::signal(SIGTERM, OnInterrupt);

	host.Run();

	Running = nullptr;
	host.Shutdown();
	return 0;
}
