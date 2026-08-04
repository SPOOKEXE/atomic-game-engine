// A thin main over the studio library. Everything here is argument parsing;
// everything after it is a call into a library.
//
// The same split `mono.client/app/main.cpp` makes, and for the same reason: a
// test binary needs something to link, and a program that is one executable's
// worth of globbed sources cannot be driven by one.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <cstdio>
#include <studio/Editor.hpp>

int main(int argc, char **argv) {
	engine::core::Log::Initialise("studio");

	engine::core::Arguments arguments("studio", "atomic studio — builds a game.");

	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag("headless", "Run with no window (needs --frames)");

	// The client's names for the same two panels. See `Options::ShowStatistics`.
	arguments.Flag("stats", "Open the statistics panel (F7)");
	arguments.Flag("graph", "Open the frame graph (F8)");

	arguments.Value("game", "PATH", "Game file to open at startup (.agame)");
	arguments.Value("width", "PX", "Window width (default 1600)");
	arguments.Value("height", "PX", "Window height (default 900)");
	arguments.Value("scale", "FACTOR", "Interface scale (default 1.0)");
	arguments.Value("tick-rate", "HZ", "Simulation ticks per second while running (default 60)");
	arguments.Value("frames", "N", "Exit after N presented frames");
	arguments.Value("capture", "PATH", "Write the viewport's world to a BMP and carry on");
	arguments.Value("profile-snapshot", "PATH", "Write a frame-graph snapshot when the run ends");
	arguments.Value("run", "MODE", "Start in edit, server or play (default edit)");
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

	studio::Options options;
	options.Width = static_cast<int>(arguments.GetInteger("width", options.Width));
	options.Height = static_cast<int>(arguments.GetInteger("height", options.Height));
	options.Scale = static_cast<float>(arguments.GetNumber("scale", options.Scale));
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.MaximumFrames = arguments.GetInteger("frames", -1);
	options.Headless = arguments.Has("headless");
	options.ShowStatistics = arguments.Has("stats");
	options.ShowFrameGraph = arguments.Has("graph");

	// A headless run has no window to close, so without a budget it would never
	// stop. Refused rather than given a default, because a default here is a
	// number nobody chose deciding how long a build server waits.
	if (options.Headless && options.MaximumFrames < 0) {
		std::fprintf(stderr, "--headless needs --frames N: there is no window to close\n");
		return 2;
	}

	if (auto game = arguments.Get("game")) {
		options.Game = std::filesystem::path(*game);
	}
	if (auto mode = arguments.Get("run")) {
		if (*mode == "play") {
			options.StartIn = studio::RunMode::Play;
		} else if (*mode == "server") {
			options.StartIn = studio::RunMode::Server;
		} else if (*mode != "edit") {
			std::fprintf(
				stderr,
				"--run: expected edit, server or play, not '%.*s'\n",
				static_cast<int>(mode->size()),
				mode->data()
			);
			return 2;
		}
	}
	if (auto capture = arguments.Get("capture")) {
		options.Capture = std::filesystem::path(*capture);
	}
	if (auto snapshot = arguments.Get("profile-snapshot")) {
		options.ProfileSnapshot = std::filesystem::path(*snapshot);
	}
	if (auto assets = arguments.Get("override-assets-directory")) {
		options.Assets = std::filesystem::path(*assets);
	}

	studio::Editor editor;
	if (!editor.Initialise(options)) {
		ENGINE_ERROR("the studio failed to start");
		return 1;
	}

	return editor.Run();
}
