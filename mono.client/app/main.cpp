// A thin main over the client library. Everything here is argument parsing;
// everything after it is a call into a library.
//
// That split is what makes single-player possible later: the client links the
// server library and starts one in-process, which cannot be done when the whole
// program is one executable's worth of globbed sources.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
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
	arguments.Flag("graph", "Open the F5 frame graph at startup");
	arguments.Flag("uncapped", "Present without waiting for vblank");
	arguments.Flag("verbose", "Log at trace level");

	arguments.Value("entities", "N", "Cubes in the demo scene (default 2048)");
	arguments.Value("tick-rate", "HZ", "Simulation ticks per second (default 60)");
	arguments.Value("frames", "N", "Exit after N presented frames");
	arguments.Value("width", "PX", "Window width (default 1280)");
	arguments.Value("height", "PX", "Window height (default 720)");
	arguments.Value("profiler-tab", "NAME", "frame, categories, systems or counters");

	arguments.Value("script", "PATH", "Luau script to run at startup (v0.6)");
	arguments.Value("enable-profiler", "SECONDS", "Wait for a Tracy profiler before starting");
	arguments.Value("profile-seconds", "SECONDS", "Run for this long, then exit");
	arguments.Value("override-assets-directory", "DIR", "Read shaders and data from here");

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

	client::Options options;
	options.Width = static_cast<int>(arguments.GetInteger("width", options.Width));
	options.Height = static_cast<int>(arguments.GetInteger("height", options.Height));
	options.Entities = static_cast<uint32_t>(arguments.GetInteger("entities", options.Entities));
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.MaximumFrames = arguments.GetInteger("frames", -1);
	options.ShowStatistics = arguments.Has("stats");
	options.ShowFrameGraph = arguments.Has("graph");
	options.Uncapped = arguments.Has("uncapped");
	options.ProfileSeconds = arguments.GetNumber("profile-seconds", 0.0);

	if (arguments.Has("enable-profiler")) {
		// Bare `--enable-profiler` is not accepted by the parser, which
		// requires a value for a Value option — so a caller who wants the
		// default writes `--enable-profiler 10`. Keeping "takes a value" and
		// "does not" as separate declarations is what makes a missing value an
		// error rather than a silently swallowed next argument.
		options.ProfilerWaitSeconds = arguments.GetNumber("enable-profiler", 10.0);
	}

	if (auto script = arguments.Get("script")) {
		options.ScriptPath = std::string(*script);
	}
	if (auto assets = arguments.Get("override-assets-directory")) {
		options.AssetsDirectory = std::filesystem::path(*assets);
	}

	if (auto tab = arguments.Get("profiler-tab")) {
		if (!ParseProfilerTab(*tab, options.Tab)) {
			std::fprintf(
				stderr, "--profiler-tab: no tab called '%.*s'\n", static_cast<int>(tab->size()), tab->data()
			);
			return 2;
		}
		// Naming a tab is asking to see it.
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
