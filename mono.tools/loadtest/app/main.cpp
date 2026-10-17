// Thin argument-parsing entry point over the load-test library.
//
// `just stress` starts a server and points this at it. Everything worth reading
// is in `loadtest/Harness.hpp`.

#include "CdnTraffic.hpp"

#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <loadtest/Harness.hpp>
#include <loadtest/Options.hpp>
#include <optional>

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
	arguments.Value("random-heading-seed", "N", "Seed repeatable random headings (default 0, disabled)");
	arguments.Value("random-heading-every-ticks", "N", "Submitted inputs per random heading (default 30)");
	arguments.Value("stall-seconds", "N", "How long a session may make no progress (default 20)");
	arguments.Value("profile-out", "PATH", "Fold this run's frame graph into a .folded flamegraph capture");
	arguments.Flag("cdn-only", "Run the signed, hash-checked HTTP content cohort instead of UDP clients");
	arguments.Value("cdn-store", "DIR", "Trusted local content store to compare with the CDN response");
	arguments.Value("cdn-publisher-key", "HEX", "Publisher public key for the signed manifest");
	arguments.Value("cdn-grant-key", "HEX", "Shared grant key for bundle requests");
	arguments.Value("cdn-address", "HOST", "CDN origin address (default 127.0.0.1)");
	arguments.Value("cdn-port", "PORT", "CDN origin HTTP port");
	arguments.Value("cdn-requests", "N", "CDN bundle requests from 5 to 1000 (default 25)");
	arguments.Value("cdn-concurrency", "N", "Maximum CDN requests in flight (default 8)");

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
	if (arguments.Has("cdn-only")) {
		const std::optional<std::string_view> store = arguments.Get("cdn-store");
		const std::optional<std::string_view> publisherKey = arguments.Get("cdn-publisher-key");
		const std::optional<std::string_view> grantKey = arguments.Get("cdn-grant-key");
		const int64_t cdnPortValue = arguments.GetInteger("cdn-port", 0);
		if (!store || !publisherKey || !grantKey || cdnPortValue < 1 || cdnPortValue > 65535) {
			std::fprintf(
				stderr,
				"--cdn-only needs --cdn-store, --cdn-publisher-key, --cdn-grant-key and a valid --cdn-port.\n"
			);
			return 2;
		}
		const std::string_view address = arguments.Get("cdn-address").value_or("127.0.0.1");
		const int64_t requestValue = arguments.GetInteger("cdn-requests", 25);
		const int64_t concurrencyValue = arguments.GetInteger("cdn-concurrency", 8);
		if (requestValue < 5 || requestValue > 1000 || concurrencyValue < 1 || concurrencyValue > 16) {
			std::fprintf(
				stderr, "--cdn-requests must be from 5 to 1000 and --cdn-concurrency from 1 to 16.\n"
			);
			return 2;
		}
		const uint16_t cdnPort = static_cast<uint16_t>(cdnPortValue);
		const uint32_t requests = static_cast<uint32_t>(requestValue);
		const uint32_t concurrency = static_cast<uint32_t>(concurrencyValue);
		engine::core::Flags::Freeze();
		std::string error;
		if (!loadtest::RunCdnTraffic(
				std::filesystem::path(*store),
				*publisherKey,
				*grantKey,
				address,
				cdnPort,
				requests,
				concurrency,
				error
			)) {
			std::fprintf(stderr, "FAIL: CDN fetch cohort: %s\n", error.c_str());
			return 1;
		}
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
	options.RandomHeadingSeed = static_cast<uint64_t>(
		arguments.GetInteger("random-heading-seed", static_cast<int64_t>(options.RandomHeadingSeed))
	);
	options.RandomHeadingEveryTicks = static_cast<uint32_t>(
		arguments.GetInteger("random-heading-every-ticks", options.RandomHeadingEveryTicks)
	);
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
	if (options.RandomHeadingSeed != 0 && options.RandomHeadingEveryTicks == 0) {
		std::fprintf(
			stderr, "--random-heading-every-ticks must be greater than zero with a random heading seed.\n"
		);
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
