// A thin main over the studio library. Everything here is argument parsing;
// everything after it is a call into a library.
//
// The same split `mono.client/app/main.cpp` makes, and for the same reason: a
// test binary needs something to link, and a program that is one executable's
// worth of globbed sources cannot be driven by one.

#include <engine/assets/ContentPolicy.hpp>
#include <engine/control/Server.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/script/ComputeJobs.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>

int main(int argc, char **argv) {
	if (engine::script::ComputeWorkerRequested(argc, argv)) {
		return engine::script::RunComputeWorker();
	}
	engine::script::ConfigureComputeWorkerProgram(argv[0]);
	engine::core::Log::Initialise("studio");

	// **The engine's settings and the content policy, and no `studio.*` table.**
	// An editor already persists its own preferences in a document it owns -
	// `studio/Config.hpp` - and a second place to say the same things is the
	// drift rule 2 is about. What a config file adds here is the settings that
	// are *not* the editor's: the log level, the job pool, and which content
	// forms this machine will decode, all three of which a studio shares with
	// the client it is previewing for.
	engine::core::Config::DeclareEngineFlags();
	engine::parallel::DeclareFlags();
	engine::assets::DeclareContentFlags(engine::assets::ContentVerb::Handle);

	engine::core::Arguments arguments("studio", "atomic studio - builds a game.");
	engine::core::Config::DeclareOptions(arguments);

	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag(
		"force-serial-compute",
		"Run every parallel dispatch on one thread, so the frame graph keeps every span"
	);
	arguments.Flag("headless", "Run with no window (needs --frames)");
	arguments.Flag("uncapped", "Draw with no frame rate ceiling (default 120 fps)");

	// The client's names for the same two panels. See `Options::ShowStatistics`.
	arguments.Flag("stats", "Open the statistics panel (F7)");
	arguments.Flag("graph", "Open and select the frame graph (F8)");
	arguments.Flag("assets", "Open the assets manager");
	arguments.Flag("viewport2", "Open the second viewport (same as --viewports 2)");

	arguments.Value("game", "PATH", "Game file to open at startup (.agame or .auniverse)");
	arguments.Value("rojo", "PATH", "Sync this Rojo project or universe at startup ($ATOMIC_ROJO_PROJECT)");
	arguments.Value("config-root", "DIR", "Keep this run's Studio configuration in DIR");
	arguments.Value("width", "PX", "Window width (default 1600)");
	arguments.Value("height", "PX", "Window height (default 900)");
	arguments.Value("scale", "FACTOR", "Interface scale (default 1.0)");
	arguments.Value("tick-rate", "HZ", "Simulation ticks per second while running (default 60)");
	arguments.Value("frames", "N", "Exit after N presented frames");
	arguments.Value("viewports", "N", "Open N viewport panels at startup (default 1)");
	arguments.Value("capture", "PATH", "Write the viewport's world to a BMP and carry on");
	arguments.Value("capture-world", "NAME", "Point --capture at this scene rather than the active one");
	arguments.Value("profile-snapshot", "PATH", "Write a frame-graph snapshot when the run ends");
	arguments.Value(
		"heap-report", "PATH", "Write a heap profile when the run ends, and sample while running"
	);
	arguments.Value("frames-in-flight", "N", "Frames the CPU may queue ahead of the GPU: 1 (default) to 3");
	arguments.Value("idle-close", "SECONDS", "Close an empty world after this long (default 300)");
	arguments.Value("run", "MODE", "Start in edit, server or play (default edit)");
	arguments.Value(
		"surface-bounces",
		"N",
		"Levels of mirror-in-mirror per frame, overriding the world (default: measured)"
	);

	// The control surface. Off unless asked for - see `Options::ControlPort`.
	//
	// **"conventionally" rather than "default", because there is no default.**
	// `core::Arguments::Value` requires a value, so `--mcp-port` on its own is a
	// parse error and no number is ever supplied for the caller. The help said
	// "default 8738" and the comment beside it described what a bare flag would
	// open, which was a shape this parser has never had. The number is still read
	// from the one constant so the text cannot drift from what `.mcp.json` dials.
	arguments.Value(
		"mcp-port",
		"PORT",
		"Listen for Model Context Protocol on 127.0.0.1:PORT (conventionally " +
			std::to_string(engine::control::DEFAULT_PORT) + ")"
	);
	arguments.Value("override-assets-directory", "DIR", "Read shaders and data from here");

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

	// Applied before the first preference, keybind, recent-project or layout
	// read. This is one override for the whole editor rather than a test mode
	// each configuration owner has to remember independently.
	if (const auto configRoot = arguments.Get("config-root")) {
		studio::SetConfigRoot(std::filesystem::path(*configRoot));
	}

	// **Before anything starts a world or a job.** The flag is read on every
	// dispatch, so setting it late would leave the frames before it with the
	// shape it exists to remove - and those are the frames somebody was
	// watching while the program came up.
	//
	// It makes the program slower on purpose. See `parallel::SetForceSerialCompute`:
	// this is a measurement instrument, and the number it produces is a serial
	// cost rather than a verdict on the parallel one.
	if (arguments.Has("force-serial-compute")) {
		engine::core::Flags::Set("engine.serial-compute", "true", engine::core::FlagSource::CommandLine);
		engine::parallel::SetForceSerialCompute(true);
		ENGINE_INFO("serial compute forced: every dispatch runs on its caller's thread");
	}

	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
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
	engine::parallel::ApplyFlags();

	// **The configured values first, then the flags over the top.** Only this
	// function can tell a flag that was given from one that was left at its
	// default - `Arguments::GetNumber(name, fallback)` answers the fallback for
	// an absent flag - so the reconciliation belongs here and nowhere else.
	// `studio/Config.hpp` states the rule: a flag is for one run and a
	// preference is a thing somebody set and expects to find again.
	studio::Preferences preferences;
	preferences.Load();

	studio::Options options;
	options.Scale = preferences.Scale;
	options.Width = static_cast<int>(arguments.GetInteger("width", options.Width));
	options.Height = static_cast<int>(arguments.GetInteger("height", options.Height));
	options.Scale = static_cast<float>(arguments.GetNumber("scale", options.Scale));
	options.ScaleAuthored = arguments.Has("scale");
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.MaximumFrames = arguments.GetInteger("frames", -1);
	// **`Has` then `GetInteger`, and the two-step is the opt-in.** A bare
	// `GetInteger` with a fallback would open the port on every run, because a
	// fallback is returned when the flag is absent. This way no flag at all
	// leaves the socket shut.
	//
	// `--mcp-port` takes a value; the parser refuses the flag without one, so
	// the fallback below is reached only for a value that is not an integer.
	// It is the configured port rather than a number written here, so somebody
	// who set one in the panel gets it back rather than a literal.
	options.ControlPort = arguments.Has("mcp-port")
							  ? static_cast<int>(arguments.GetInteger("mcp-port", preferences.ControlPort))
							  : -1;
	options.SurfaceBounces = static_cast<int>(arguments.GetInteger("surface-bounces", 0));
	options.Headless = arguments.Has("headless");
	options.Uncapped = arguments.Has("uncapped");
	// A flag turns a panel on for this run; the file remembers what was left
	// open. Neither can turn the other's off, which is the honest reading of two
	// booleans that both mean "show it".
	options.ShowStatistics = arguments.Has("stats") || preferences.ShowStatistics;
	options.ShowFrameGraph = arguments.Has("graph") || preferences.ShowFrameGraph;
	options.FocusFrameGraph = arguments.Has("graph");
	options.ShowAssetsPanel = arguments.Has("assets") || preferences.ShowAssets;
	// **`--viewport2` is the old spelling of `--viewports 2` and still works.**
	// Whichever asks for more wins, because both mean "open at least this many"
	// and neither can sensibly close a panel the other opened.
	//
	// The floor of one is not politeness about bad input: `StartViewports` is
	// unsigned, so a negative would arrive as a request for four billion panels.
	options.StartViewports = static_cast<size_t>(
		std::max<int64_t>({arguments.GetInteger("viewports", 1), arguments.Has("viewport2") ? 2 : 1, 1})
	);

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

		// **A headless capture pins the animation clock, and it is not a flag.**
		// The only reason to take one is to compare it with another, and a clock
		// accumulated from the measured frame delta lands frame N on a different
		// phase every run - two captures of one unchanged world differ by about
		// an eighth of their bytes, which swamps most changes worth checking.
		//
		// Headless only, because a capture taken while somebody is watching is a
		// screenshot of what they are watching, and pinning the clock there would
		// make the picture disagree with the window it came from.
		if (options.Headless) {
			options.FixedAnimationStep = 1.0 / 60.0;
		}
	}
	if (auto snapshot = arguments.Get("profile-snapshot")) {
		options.ProfileSnapshot = std::filesystem::path(*snapshot);
	}
	if (auto report = arguments.Get("heap-report")) {
		options.HeapReport = std::filesystem::path(*report);
	}
	options.FramesInFlight =
		static_cast<int>(arguments.GetInteger("frames-in-flight", options.FramesInFlight));
	options.IdleCloseSeconds =
		static_cast<float>(arguments.GetNumber("idle-close", options.IdleCloseSeconds));
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
