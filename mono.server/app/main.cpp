// A thin main over the server library, for the same reason the client's is
// thin: single-player links this library and hosts a server in-process, which
// is impossible when the program is one executable's worth of globbed sources.

#include <server/Server.hpp>

#include <engine/core/Arguments.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>

#include <csignal>
#include <cstdio>

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

	arguments.Value("tick-rate", "HZ", "Ticks per second (default 30)");
	arguments.Value("entities", "N", "Entities in the placeholder world (default 4096)");
	arguments.Value("ticks", "N", "Exit after N ticks");
	arguments.Value("seconds", "N", "Exit after N seconds");
	arguments.Value("game", "PATH", "Game file to host (v0.2+)");
	arguments.Value("override-assets-directory", "DIR", "Read staged data from here");

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

	if (auto game = arguments.Get("game")) {
		options.GamePath = std::string(*game);
	}
	if (auto assets = arguments.Get("override-assets-directory")) {
		options.AssetsDirectory = std::filesystem::path(*assets);
	}

	engine::core::FrameGraph::SetEnabled(arguments.Has("graph"));

	server::Server host;
	if (!host.Initialise(options)) {
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
