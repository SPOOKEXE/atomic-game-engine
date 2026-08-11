// A thin main over the studio library. Everything here is argument parsing;
// everything after it is a call into a library.
//
// The same split `mono.client/app/main.cpp` makes, and for the same reason: a
// test binary needs something to link, and a program that is one executable's
// worth of globbed sources cannot be driven by one.

#include <cstdlib>
#include <engine/parallel/Jobs.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <cstdio>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>

int main(int argc, char **argv) {
	engine::core::Log::Initialise("studio");

	engine::core::Arguments arguments("studio", "atomic studio — builds a game.");

	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag(
		"force-serial-compute",
		"Run every parallel dispatch on one thread, so the frame graph keeps every span"
	);
	arguments.Flag("headless", "Run with no window (needs --frames)");
	arguments.Flag("uncapped", "Draw with no frame rate ceiling (default 120 fps)");

	// The client's names for the same two panels. See `Options::ShowStatistics`.
	arguments.Flag("stats", "Open the statistics panel (F7)");
	arguments.Flag("graph", "Open the frame graph (F8)");
	arguments.Flag("assets", "Open the assets manager");
	arguments.Flag("viewport2", "Open the second viewport");

	arguments.Value("game", "PATH", "Game file to open at startup (.agame)");
	arguments.Value("rojo", "PATH", "Sync this Rojo project or universe at startup ($ATOMIC_ROJO_PROJECT)");
	arguments.Value("width", "PX", "Window width (default 1600)");
	arguments.Value("height", "PX", "Window height (default 900)");
	arguments.Value("scale", "FACTOR", "Interface scale (default 1.0)");
	arguments.Value("tick-rate", "HZ", "Simulation ticks per second while running (default 60)");
	arguments.Value("frames", "N", "Exit after N presented frames");
	arguments.Value("capture", "PATH", "Write the viewport's world to a BMP and carry on");
	arguments.Value("capture-world", "NAME", "Point --capture at this scene rather than the active one");
	arguments.Value("profile-snapshot", "PATH", "Write a frame-graph snapshot when the run ends");
	arguments.Value("idle-close", "SECONDS", "Close an empty world after this long (default 300)");
	arguments.Value("run", "MODE", "Start in edit, server or play (default edit)");

	// The control surface. Off unless asked for — see `Options::ControlPort`.
	arguments.Value("mcp-port", "PORT", "Listen for Model Context Protocol on 127.0.0.1:PORT (default 8738)");
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

	// **Before anything starts a world or a job.** The flag is read on every
	// dispatch, so setting it late would leave the frames before it with the
	// shape it exists to remove — and those are the frames somebody was
	// watching while the program came up.
	//
	// It makes the program slower on purpose. See `parallel::SetForceSerialCompute`:
	// this is a measurement instrument, and the number it produces is a serial
	// cost rather than a verdict on the parallel one.
	if (arguments.Has("force-serial-compute")) {
		engine::parallel::SetForceSerialCompute(true);
		ENGINE_INFO("serial compute forced: every dispatch runs on its caller's thread");
	}

	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
	}

	// **The configured values first, then the flags over the top.** Only this
	// function can tell a flag that was given from one that was left at its
	// default — `Arguments::GetNumber(name, fallback)` answers the fallback for
	// an absent flag — so the reconciliation belongs here and nowhere else.
	// `studio/Config.hpp` states the rule: a flag is for one run and a
	// preference is a thing somebody set and expects to find again.
	studio::Preferences preferences;
	preferences.Load();

	studio::Options options;
	options.Scale = preferences.Scale;
	options.Width = static_cast<int>(arguments.GetInteger("width", options.Width));
	options.Height = static_cast<int>(arguments.GetInteger("height", options.Height));
	options.Scale = static_cast<float>(arguments.GetNumber("scale", options.Scale));
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.MaximumFrames = arguments.GetInteger("frames", -1);
	// **`Has` then `GetInteger`, and the two-step is the opt-in.** A bare
	// `GetInteger` with a fallback would open the port on every run, because a
	// fallback is returned when the flag is absent. This way `--mcp-port` alone
	// takes this program's number and no flag at all leaves it shut.
	// The bare flag takes the configured port rather than a number written
	// here, so somebody who set one in the panel gets it back from the
	// command line too. No flag at all still leaves the socket shut.
	options.ControlPort =
		arguments.Has("mcp-port")
			? static_cast<int>(arguments.GetInteger("mcp-port", preferences.ControlPort))
			: -1;
	options.Headless = arguments.Has("headless");
	options.Uncapped = arguments.Has("uncapped");
	// A flag turns a panel on for this run; the file remembers what was left
	// open. Neither can turn the other's off, which is the honest reading of two
	// booleans that both mean "show it".
	options.ShowStatistics = arguments.Has("stats") || preferences.ShowStatistics;
	options.ShowFrameGraph = arguments.Has("graph") || preferences.ShowFrameGraph;
	options.ShowAssetsPanel = arguments.Has("assets") || preferences.ShowAssets;
	options.ShowSecondViewport = arguments.Has("viewport2");

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

	// **The flag, or the variable when there is no flag.** Same reconciliation
	// as every other option on this page and in the same direction: what
	// somebody typed for this run wins over what their shell has been carrying
	// all day. Nothing writes the variable back, so `--rojo` cannot make an
	// environment sticky.
	if (auto project = arguments.Get("rojo")) {
		options.RojoProject = std::filesystem::path(*project);
	} else if (const char *environment = std::getenv("ATOMIC_ROJO_PROJECT");
			   environment != nullptr && environment[0] != '\0') {
		options.RojoProject = std::filesystem::path(environment);
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
	if (auto world = arguments.Get("capture-world")) {
		options.CaptureWorld = *world;
	}
	if (auto capture = arguments.Get("capture")) {
		options.Capture = std::filesystem::path(*capture);
	}
	if (auto snapshot = arguments.Get("profile-snapshot")) {
		options.ProfileSnapshot = std::filesystem::path(*snapshot);
	}
	options.IdleCloseSeconds = static_cast<float>(arguments.GetNumber("idle-close", options.IdleCloseSeconds));
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
