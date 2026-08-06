// Thin argument-parsing entry point over the client library.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/DebugPanels.hpp>

#include <cctype>
#include <client/Client.hpp>
#include <cstdio>
#include <string>

namespace {

	// Matches a tab by name, case-insensitively, so `--profiler-tab systems`
	// does not depend on how the enum happens to be spelled.
	bool ParseProfilerTab(std::string_view given, engine::render::ProfilerTab &out) {
		std::string wanted(given);
		for (char &character : wanted) {
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}

		for (uint8_t index = 0; index < static_cast<uint8_t>(engine::render::ProfilerTab::Count); index++) {
			const auto candidate = static_cast<engine::render::ProfilerTab>(index);
			if (engine::render::GetProfilerTabName(candidate) == wanted) {
				out = candidate;
				return true;
			}
		}
		return false;
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("client");

	engine::core::Arguments arguments("client", "atomic — runs a game.");

	arguments.Flag("stats", "Open the F3 statistics panel at startup");
	arguments.Flag("net", "Open the F4 network panel at startup (needs --connect)");
	arguments.Flag("graph", "Open the F5 frame graph at startup");
	arguments.Flag("uncapped", "Present without waiting for vblank");
	arguments.Value("max-fps", "N", "Hold this frame rate. Needs --uncapped; 0 is no limit");
	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag(
		"force-serial-compute",
		"Run every parallel dispatch on one thread, so the frame graph keeps every span"
	);

	arguments.Value("entities", "N", "Cubes in the demo scene, per world (default 2048)");
	arguments.Value("worlds", "N", "Worlds to simulate and composite (default 1)");
	arguments.Value("view-spacing", "UNITS", "World units between composited views (default 40)");
	arguments.Value("tick-rate", "HZ", "Simulation ticks per second (default 60)");
	arguments.Value("frames", "N", "Exit after N presented frames");
	arguments.Value("width", "PX", "Window width (default 1280)");
	arguments.Value("height", "PX", "Window height (default 720)");
	arguments.Value("profiler-tab", "NAME", "frame, categories, systems or counters");

	arguments.Value("script", "PATH", "Luau script to run at startup (v0.6)");
	arguments.Value("game", "PATH", "Game file to play single-player (.agame)");
	arguments.Value("enable-profiler", "SECONDS", "Wait for a Tracy profiler before starting");
	arguments.Value("profile-seconds", "SECONDS", "Run for this long, then exit");
	arguments.Value("override-assets-directory", "DIR", "Read shaders and data from here");
	arguments.Value("connect", "HOST:PORT", "Replicate a world from this server, beside the demo");
	arguments.Value(
		"server-key",
		"HEX",
		"64 hex characters — the server identity to pin. Without it a relay in the path can read "
		"everything"
	);
	arguments.Value(
		"cdn", "HOST:PORT", "A content origin, in priority order. 'dir:PATH' for a local store. Repeatable"
	);
	arguments.Value("content-cache", "DIR", "Keep verified content here between runs");
	arguments.Value("publisher-key", "HEX", "64 hex characters — the key whose manifests this client trusts");
	arguments.Value("sound", "PATH", "Play this .wav or .mp3 on a loop — proves audio runs in-game");
	arguments.Value(
		"capture",
		"PATH",
		"Write a BMP of the scene near the end of the run. Needs --frames; renders offscreen"
	);

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}

	// Set before startup so every dispatch uses the measured serial path.
	if (arguments.Has("force-serial-compute")) {
		engine::parallel::SetForceSerialCompute(true);
		ENGINE_INFO("serial compute forced: every dispatch runs on its caller's thread");
	}

	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
	}

	client::Options options;
	options.Width = static_cast<int>(arguments.GetInteger("width", options.Width));
	options.Height = static_cast<int>(arguments.GetInteger("height", options.Height));
	options.Entities = static_cast<uint32_t>(arguments.GetInteger("entities", options.Entities));
	options.Worlds = static_cast<uint32_t>(arguments.GetInteger("worlds", options.Worlds));
	options.ViewSpacing = static_cast<float>(arguments.GetNumber("view-spacing", options.ViewSpacing));
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.MaximumFrames = arguments.GetInteger("frames", -1);
	options.ShowStatistics = arguments.Has("stats");
	options.ShowNetwork = arguments.Has("net");
	options.ShowFrameGraph = arguments.Has("graph");
	options.Uncapped = arguments.Has("uncapped");
	options.MaximumFrameRate =
		static_cast<uint32_t>(arguments.GetInteger("max-fps", options.MaximumFrameRate));
	options.ProfileSeconds = arguments.GetNumber("profile-seconds", 0.0);

	if (arguments.Has("enable-profiler")) {
		options.ProfilerWaitSeconds = arguments.GetNumber("enable-profiler", 10.0);
	}

	if (auto script = arguments.Get("script")) {
		options.ScriptPath = std::string(*script);
	}
	if (auto game = arguments.Get("game")) {
		options.GameFile = std::filesystem::path(*game);

		// Loudly rather than silently. A run given both a game file and a scene
		// script has to choose, and a choice nobody was told about is a run
		// that did something other than what was asked.
		if (!options.ScriptPath.empty()) {
			ENGINE_WARN("--game and --script were both given; playing the game file");
			options.ScriptPath.clear();
		}
	}
	if (auto assets = arguments.Get("override-assets-directory")) {
		options.AssetsDirectory = std::filesystem::path(*assets);
	}
	if (auto server = arguments.Get("connect")) {
		options.ConnectAddress = std::string(*server);
	}
	if (auto key = arguments.Get("server-key")) {
		options.ServerKey = std::string(*key);
	}

	for (const std::string_view source : arguments.GetAll("cdn")) {
		options.ContentSources.emplace_back(source);
	}
	if (auto cache = arguments.Get("content-cache")) {
		options.ContentCache = std::filesystem::path(*cache);
	}
	if (auto capture = arguments.Get("capture")) {
		options.Capture = std::filesystem::path(*capture);
	}
	if (auto sound = arguments.Get("sound")) {
		options.SoundPath = std::filesystem::path(*sound);
	}
	if (auto key = arguments.Get("publisher-key")) {
		options.ContentPublisherKey = std::string(*key);
	}

	if (auto tab = arguments.Get("profiler-tab")) {
		if (!ParseProfilerTab(*tab, options.Tab)) {
			std::fprintf(
				stderr, "--profiler-tab: no tab called '%.*s'\n", static_cast<int>(tab->size()), tab->data()
			);
			return 2;
		}
		options.ShowFrameGraph = true;
	}

	// A profiling run wants the graph collecting, or it measures nothing.
	if (options.ProfileSeconds > 0.0) {
		options.ShowFrameGraph = true;
	}

	client::Client client;
	if (!client.Initialise(options)) {
		ENGINE_ERROR("client failed to start");
		return 1;
	}

	return client.Run();
}
