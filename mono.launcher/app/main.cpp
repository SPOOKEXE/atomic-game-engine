// A thin main over the launcher library. Everything here is argument parsing.
//
// The same split every other program in the tree makes, and for the same
// reason: a test binary needs something to link, and a program that is one
// executable's worth of globbed sources cannot be driven by one.

#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <cstdio>
#include <filesystem>
#include <launcher/Launcher.hpp>

int main(int argc, char **argv) {
	engine::core::Log::Initialise("launcher");

	// **The engine's settings and no `launcher.*` table.** This program has no
	// simulation, no content and no jobs; the settings it would declare are the
	// ones it is about to hand to a child, and those belong to the child's own
	// table. What a config file adds here is the log level, which is shared by
	// everything in the tree.
	engine::core::Config::DeclareEngineFlags();

	engine::core::Arguments arguments("launcher", "atomic - picks what to run and runs it.");
	engine::core::Config::DeclareOptions(arguments);

	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag("headless", "Run with no window (needs --frames)");
	arguments.Value("mode", "NAME", "Open straight onto a mode: play, join, host, studio or cdn");
	arguments.Value("width", "PX", "Window width (default 1100)");
	arguments.Value("height", "PX", "Window height (default 720)");
	arguments.Value("scale", "FACTOR", "Interface scale (default 1.0)");
	arguments.Value("frames", "N", "Exit after N presented frames");
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

	launcher::Options options;
	options.Width = static_cast<int>(arguments.GetInteger("width", options.Width));
	options.Height = static_cast<int>(arguments.GetInteger("height", options.Height));
	options.Scale = static_cast<float>(arguments.GetNumber("scale", options.Scale));
	options.Headless = arguments.Has("headless");
	options.MaximumFrames = arguments.GetInteger("frames", -1);

	if (const auto mode = arguments.Get("mode")) {
		options.StartMode = std::string(*mode);
	}
	if (const auto assets = arguments.Get("override-assets-directory")) {
		options.Assets = std::filesystem::path(*assets);
	}

	// A headless run has no window to close, so without a budget it would never
	// stop. Refused rather than given a default, for the studio's reason: a
	// default here is a number nobody chose deciding how long a build server
	// waits.
	if (options.Headless && options.MaximumFrames < 0) {
		std::fprintf(stderr, "--headless needs --frames N: there is no window to close\n");
		return 2;
	}

	launcher::Launcher launcher;
	if (!launcher.Initialise(options)) {
		ENGINE_ERROR("the launcher failed to start");
		return 1;
	}

	return launcher.Run();
}
