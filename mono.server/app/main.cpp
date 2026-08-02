// A thin main over the server library, for the same reason the client's is
// thin: single-player links this library and hosts a server in-process, which
// is impossible when the program is one executable's worth of globbed sources.

#include <engine/core/Arguments.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>

#include <csignal>
#include <cstdio>
#include <server/Server.hpp>

namespace {
	server::Server *Running = nullptr;

	// Ctrl-C has to reach the tick loop rather than killing the process where
	// it stands. Only Stop is called from here — it is one atomic store, which
	// is about the limit of what is safe in a signal handler.
	extern "C" void OnInterrupt(int) {
		if (Running) {
			Running->Stop();
		}
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("server");

	engine::core::Arguments arguments("server", "atomic — hosts a game.");

	arguments.Flag("unpaced", "Tick back to back instead of pacing to the tick rate");
	arguments.Flag("graph", "Collect the frame graph (for a Tracy capture)");
	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag("chatter", "Make every world publish on a shared topic (no game file yet)");

	arguments.Value("tick-rate", "HZ", "Ticks per second (default 30)");
	arguments.Value("entities", "N", "Entities in the placeholder world (default 4096)");
	arguments.Value("ticks", "N", "Exit after N ticks");
	arguments.Value("seconds", "N", "Exit after N seconds");
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

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}

	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
	}

	server::Options options;
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.Entities = static_cast<uint32_t>(arguments.GetInteger("entities", options.Entities));
	options.MaximumTicks = arguments.GetInteger("ticks", -1);
	options.Seconds = arguments.GetNumber("seconds", 0.0);
	options.Unpaced = arguments.Has("unpaced");
	options.Chatter = arguments.Has("chatter");

	if (auto game = arguments.Get("game")) {
		options.GamePath = std::string(*game);
	}
	if (auto record = arguments.Get("record")) {
		options.RecordPath = std::filesystem::path(*record);
	}
	if (auto replay = arguments.Get("replay")) {
		options.ReplayPath = std::filesystem::path(*replay);
	}
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

	if (!options.HostName.empty() && !options.RemoteWorlds.empty()) {
		// A host that spawned hosts of its own would build a tree nobody
		// planned and nothing supervises above the first level.
		std::fprintf(stderr, "--host and --remote-world are for opposite ends of a link.\n");
		return 2;
	}

	engine::core::FrameGraph::SetEnabled(arguments.Has("graph"));

	server::Server host;
	if (!host.Initialise(options)) {
		// Shutdown even on failure. Initialise may already have started the job
		// pool, and leaving its threads alive means the process never exits —
		// which reads as a hang rather than as the error it is. Shutdown is
		// documented as safe after a failed Initialise for exactly this.
		host.Shutdown();
		ENGINE_ERROR("server failed to start");
		return 1;
	}

	Running = &host;
	std::signal(SIGINT, OnInterrupt);
	std::signal(SIGTERM, OnInterrupt);

	host.Run();

	Running = nullptr;
	host.Shutdown();
	return 0;
}
