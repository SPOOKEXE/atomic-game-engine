// Thin argument-parsing entry point over the load-test library.
//
// `just stress` starts a server and points this at it. Everything worth reading
// is in `loadtest/Harness.hpp`.

#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <cstdio>
#include <loadtest/Harness.hpp>
#include <loadtest/Options.hpp>

int main(int argc, char **argv) {
	engine::core::Log::Initialise("loadtest");

	// Declared before anything is parsed, so `--flags` can list them and a
	// config file naming one is not an error.
	engine::core::Config::DeclareEngineFlags();
	loadtest::DeclareFlags();

	engine::core::Arguments arguments("loadtest", "atomic - opens many real clients against one server.");
	engine::core::Config::DeclareOptions(arguments);

	arguments.Flag("verbose", "Log at trace level");
	arguments.Value("clients", "N", "How many virtual clients to open (default 200)");
	arguments.Value("address", "HOST", "The server's address (default 127.0.0.1)");
	arguments.Value("port", "PORT", "The server's UDP port");
	arguments.Value("tick-rate", "HZ", "How fast to tick the clients (default 30)");
	arguments.Value("seconds", "N", "Run for this long");
	arguments.Value("ticks", "N", "Run this many ticks");
	arguments.Value("connects-per-tick", "N", "How many sessions may start dialling on one tick (default 8)");
	arguments.Value("input-every-ticks", "N", "How often a client submits an input (default 1)");
	arguments.Value("stall-seconds", "N", "How long a session may make no progress (default 20)");
	arguments.Value("profile-out", "PATH", "Fold this run's frame graph into a .folded flamegraph capture");

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

	const engine::core::ConfigReport settings = engine::core::Config::Apply(arguments);
	if (!settings.Ok) {
		std::fprintf(stderr, "%s\n", settings.Error.c_str());
		return 2;
	}
	if (engine::core::Config::ListingWanted(arguments)) {
		std::fputs(engine::core::Flags::Listing().c_str(), stdout);
		return 0;
	}

	// The settings first, the command line over the top - the precedence every
	// program here expresses.
	loadtest::Options options = loadtest::OptionsFromFlags();
	options.Clients = static_cast<uint32_t>(arguments.GetInteger("clients", options.Clients));
	if (auto address = arguments.Get("address")) {
		options.Address = std::string(*address);
	}
	options.Port = static_cast<uint16_t>(arguments.GetInteger("port", options.Port));
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.Seconds = arguments.GetNumber("seconds", options.Seconds);
	options.Ticks = arguments.GetInteger("ticks", options.Ticks);
	options.ConnectsPerTick =
		static_cast<uint32_t>(arguments.GetInteger("connects-per-tick", options.ConnectsPerTick));
	options.InputEveryTicks =
		static_cast<uint32_t>(arguments.GetInteger("input-every-ticks", options.InputEveryTicks));
	options.StallSeconds = arguments.GetNumber("stall-seconds", options.StallSeconds);
	if (auto profile = arguments.Get("profile-out")) {
		options.ProfilePath = std::filesystem::path(*profile);
	}

	if (options.Port == 0) {
		std::fprintf(stderr, "--port names the server's UDP port, and it is not optional.\n");
		return 2;
	}

	// **Refused rather than defaulted.** A run with no budget at all ends only
	// when somebody kills it, and what it would leave behind is a report nobody
	// receives - the numbers are printed at the end.
	if (options.Seconds <= 0.0 && options.Ticks <= 0) {
		std::fprintf(stderr, "give --seconds or --ticks; a run with neither never ends.\n");
		return 2;
	}

	engine::core::Flags::Freeze();

	loadtest::Harness harness(options);
	const loadtest::Summary summary = harness.Run();

	std::fputs(loadtest::Describe(summary).c_str(), stdout);

	if (harness.Unopened() > 0) {
		// A socket per session, so this is usually the file-descriptor limit.
		// Said loudly, because the alternative is a run that quietly measured
		// half the clients it was asked for.
		std::fprintf(
			stderr, "WARNING: %zu socket(s) could not be opened - check `ulimit -n`.\n", harness.Unopened()
		);
		return 1;
	}

	// A run where nobody joined is a failed run, not a slow one. The recipe
	// reads this rather than grepping the report.
	if (summary.Playing == 0) {
		std::fprintf(stderr, "FAIL: no session joined the world.\n");
		return 1;
	}

	return 0;
}
