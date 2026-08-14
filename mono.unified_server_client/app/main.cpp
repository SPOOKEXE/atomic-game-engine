// A thin main over the harness. Everything here is argument parsing and
// printing; everything after it is a call into a library.
//
// The output is one line per tick and a summary, both meant to be read by a
// person looking for the stage that lost the world. The columns are the four
// stages in order - produced, sent, applied, drawn - so the first one that goes
// wrong is the first one whose column stops making sense.

#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unified/Harness.hpp>
#include <vector>

namespace {

	// Parses a comma-separated list of message ordinals.
	//
	// @param given The text after `--drop`.
	// @param out   Where the ordinals go.
	// @return `false` when a field is not a number.
	bool ParseOrdinals(std::string_view given, std::vector<uint64_t> &out) {
		size_t start = 0;
		while (start <= given.size()) {
			const size_t comma = given.find(',', start);
			const std::string_view field =
				given.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
			if (field.empty()) {
				return false;
			}

			char *end = nullptr;
			const std::string text(field);
			const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
			if (end == text.c_str() || *end != '\0') {
				return false;
			}
			out.push_back(static_cast<uint64_t>(value));

			if (comma == std::string_view::npos) {
				break;
			}
			start = comma + 1;
		}
		return true;
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("unified");

	// **The engine's settings and no `unified.*` table.** This is a diagnostic
	// harness whose every option describes one run - how many ticks, which
	// ordinals to drop - and a run's shape is not a deployment's decision.
	engine::core::Config::DeclareEngineFlags();
	engine::parallel::DeclareFlags();

	engine::core::Arguments arguments(
		"unified_server_client",
		"atomic - a server and a client in one process, with no network between them."
	);
	engine::core::Config::DeclareOptions(arguments);

	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag(
		"force-serial-compute",
		"Run every parallel dispatch on one thread, so the frame graph keeps every span"
	);
	arguments.Flag("quiet", "Print the summary only, not a line per tick");

	arguments.Value("entities", "N", "Entities in the placeholder world (default 64)");
	arguments.Value("scene", "PATH", "Author the server's world from a scene script");
	arguments.Value("ticks", "N", "Ticks to run after the join (default 120)");
	arguments.Value("tick-rate", "HZ", "The authority's tick rate (default 30)");
	arguments.Value("frames-per-tick", "N", "Frames drawn per tick (default 4)");
	arguments.Value("delay-ticks", "N", "The snapshot buffer's delay (default 2)");
	arguments.Value("drop", "N,N,...", "Message ordinals to lose silently");

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

	const engine::core::ConfigReport configured = engine::core::Config::Apply(arguments);
	if (!configured.Ok) {
		std::fprintf(stderr, "%s\n", configured.Error.c_str());
		return 2;
	}
	if (engine::core::Config::ListingWanted(arguments)) {
		std::fputs(engine::core::Flags::Listing().c_str(), stdout);
		return 0;
	}
	engine::parallel::ApplyFlags();

	unified::Settings settings;
	settings.Entities = static_cast<uint32_t>(arguments.GetInteger("entities", settings.Entities));
	if (auto scene = arguments.Get("scene")) {
		settings.ScenePath = std::string(*scene);
	}
	settings.TickRate = arguments.GetNumber("tick-rate", settings.TickRate);
	settings.FramesPerTick =
		static_cast<int>(arguments.GetInteger("frames-per-tick", settings.FramesPerTick));
	settings.Interpolation.DelayTicks = arguments.GetNumber("delay-ticks", settings.Interpolation.DelayTicks);

	if (auto drop = arguments.Get("drop")) {
		if (!ParseOrdinals(*drop, settings.Drop)) {
			std::fprintf(stderr, "--drop takes message ordinals, comma separated: --drop 12,13\n");
			return 2;
		}
	}

	const auto ticks = static_cast<uint64_t>(arguments.GetInteger("ticks", 120));
	const bool quiet = arguments.Has("quiet");

	unified::Harness harness(settings);

	ENGINE_INFO(
		"{} entities at {:.0f} Hz, {} frame(s) per tick, {:.1f} tick(s) of interpolation delay",
		settings.Entities,
		settings.TickRate,
		settings.FramesPerTick,
		settings.Interpolation.DelayTicks
	);

	if (!harness.Join()) {
		// **The first thing this program is for.** A join that never completes
		// with no network in the way is a snapshot the replica refused, and the
		// replica's own counters say which kind - not a lost datagram, because
		// there are none.
		ENGINE_ERROR(
			"the client never joined: {} snapshot(s), {} malformed, {} stale",
			harness.Replica().Stats().Snapshots,
			harness.Replica().Stats().Malformed,
			harness.Replica().Stats().Stale
		);
		return 1;
	}

	ENGINE_INFO(
		"joined at tick {} after {} message(s): {} entities on the server, {} on the client",
		harness.Replica().Applied(),
		harness.Handed(),
		harness.ServerWorld().CountMatching<engine::scene::Transform>(),
		harness.ClientWorld().CountMatching<engine::scene::Transform>()
	);

	if (!quiet) {
		std::printf(
			"\n%8s %5s %8s %7s %8s %7s %7s %6s %10s %10s %10s %8s %7s\n",
			"tick",
			"msgs",
			"bytes",
			"largest",
			"applied",
			"srv-ent",
			"cli-ent",
			"drawn",
			"server-x",
			"client-x",
			"drawn-x",
			"behind",
			"frozen"
		);
	}

	uint64_t frozen = 0;
	uint64_t frames = 0;
	uint64_t largest = 0;
	unified::Report last;

	for (uint64_t step = 0; step < ticks; step++) {
		last = harness.Step();

		frozen += static_cast<uint64_t>(last.FrozenFrames);
		frames += static_cast<uint64_t>(settings.FramesPerTick);
		largest = std::max<uint64_t>(largest, last.LargestMessage);

		if (!quiet) {
			std::printf(
				"%8llu %5zu %8zu %7zu %8llu %7zu %7zu %6zu %10.3f %10.3f %10.3f %8.2f %7d\n",
				static_cast<unsigned long long>(last.Tick),
				last.Messages,
				last.Bytes,
				last.LargestMessage,
				static_cast<unsigned long long>(last.Applied),
				last.ServerEntities,
				last.ClientEntities,
				last.Drawn,
				static_cast<double>(last.ServerX),
				static_cast<double>(last.ClientX),
				static_cast<double>(last.DrawnX),
				last.Behind,
				last.FrozenFrames
			);
		}
	}

	// **The three numbers worth reading, and each names a different failure.**
	std::printf("\n");
	std::printf(
		"entities   server %zu · client %zu · drawn %zu\n",
		last.ServerEntities,
		last.ClientEntities,
		last.Drawn
	);
	std::printf("largest message  %llu bytes\n", static_cast<unsigned long long>(largest));
	std::printf(
		"frozen frames    %llu of %llu (%.1f%%)\n",
		static_cast<unsigned long long>(frozen),
		static_cast<unsigned long long>(frames),
		frames == 0 ? 0.0 : 100.0 * static_cast<double>(frozen) / static_cast<double>(frames)
	);

	// A world with rows in it and nothing in the draw list is the reported
	// symptom, so it is an exit code rather than a line somebody has to notice.
	if (last.ClientEntities > 0 && last.Drawn == 0) {
		ENGINE_ERROR("the client holds {} entities and drew none of them", last.ClientEntities);
		return 1;
	}
	if (last.ClientEntities != last.ServerEntities) {
		ENGINE_ERROR(
			"the client holds {} entities and the server has {}", last.ClientEntities, last.ServerEntities
		);
		return 1;
	}

	return 0;
}
