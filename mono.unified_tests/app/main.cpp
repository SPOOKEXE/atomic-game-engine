// A thin main over the crossings. Everything here is argument parsing and
// printing; everything after it is a call into a library.
//
// The output is one line per tick and then every module's own report, both
// meant to be read by a person looking for the stage that lost the world. The
// columns are the four stages in order - produced, sent, applied, drawn - so
// the first one that goes wrong is the first one whose column stops making
// sense, and the report underneath says which two modules disagree about it.

#include <engine/core/Arguments.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Log.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unified/Arrangement.hpp>
#include <unified/Crossing.hpp>
#include <unified/Reports.hpp>
#include <vector>

namespace {

	using engine::core::HeapProfile;

	// What a run answers with when a tag climbed past `--heap-growth-limit`.
	//
	// Apart from 1, which is a world that did not cross, because the two want
	// different fixes and a soak script has to tell them apart.
	constexpr int EXIT_RUNAWAY_HEAP = 3;

	// The least window a slope may be fitted to.
	//
	// Under this a level load and a leak are the same two points.
	constexpr double MINIMUM_GROWTH_WINDOW_SECONDS = 6.0;

	// Parses a comma-separated list of ordinals.
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

	// Every arrangement's name, for the help text and for a parse failure.
	std::string Arrangements() {
		std::string names;
		for (const unified::Arrangement &arrangement : unified::AllArrangements()) {
			if (!names.empty()) {
				names += ", ";
			}
			names += arrangement.Name();
		}
		return names;
	}

	// What one arrangement did, and whether its modules agreed.
	struct Outcome {
		unified::Arrangement Ran;
		size_t Contradictions = 0;
		bool Joined = false;
	};

	// Runs one arrangement to completion and prints what it found.
	//
	// @param settings    How big and how fast.
	// @param arrangement What goes between the halves.
	// @param ticks       Ticks to run after the join, when `seconds` is zero.
	// @param seconds     Wall clock to run for instead, or zero for `ticks`.
	// @param quiet       Whether to skip the per-tick table.
	// @param sampling    Whether to take heap readings as it goes.
	// @return What happened.
	Outcome RunOne(
		const unified::Settings &settings,
		const unified::Arrangement &arrangement,
		uint64_t ticks,
		double seconds,
		bool quiet,
		bool sampling
	) {
		Outcome outcome;
		outcome.Ran = arrangement;

		ENGINE_INFO(
			"{}: {} entities at {:.0f} Hz, {} frame(s) per tick, {:.1f} tick(s) of interpolation delay",
			arrangement.Name(),
			settings.Entities,
			settings.TickRate,
			settings.FramesPerTick,
			settings.Interpolation.DelayTicks
		);

		unified::Crossing crossing(settings, arrangement);

		if (!crossing.Join()) {
			// **The first thing this program is for.** A join that never
			// completes is a snapshot the replica refused, and the replica's own
			// counters say which kind.
			ENGINE_ERROR(
				"{}: the client never joined: {} snapshot(s), {} malformed, {} stale",
				arrangement.Name(),
				crossing.Replica().Stats().Snapshots,
				crossing.Replica().Stats().Malformed,
				crossing.Replica().Stats().Stale
			);
			return outcome;
		}
		outcome.Joined = true;

		ENGINE_INFO(
			"{}: joined at tick {} after {} message(s)",
			arrangement.Name(),
			crossing.Replica().Applied(),
			crossing.Handed()
		);

		if (!quiet) {
			std::printf(
				"\n%8s %5s %8s %7s %8s %7s %7s %6s %10s %10s %10s %8s %7s %6s\n",
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
				"frozen",
				"routes"
			);
		}

		uint64_t frozen = 0;
		uint64_t frames = 0;

		// **The one place this program reads a clock, and only when asked to.**
		// Every number it reports is a function of the settings, so a run is
		// reproducible from them - except a heap slope, which is bytes per
		// *second* and cannot be fitted to a tick count. `--seconds` is for
		// that and for nothing else; it makes the tick count of a run a
		// property of the machine.
		const double until = seconds > 0.0 ? engine::core::Clock::Seconds() + seconds : 0.0;

		for (uint64_t step = 0; seconds > 0.0 ? engine::core::Clock::Seconds() < until : step < ticks;
			 step++) {
			const unified::Report last = crossing.Step();

			frozen += static_cast<uint64_t>(last.FrozenFrames);
			frames += static_cast<uint64_t>(settings.FramesPerTick);

			if (sampling) {
				HeapProfile::SampleIfDue();
			}

			if (!quiet) {
				std::printf(
					"%8llu %5zu %8zu %7zu %8llu %7zu %7zu %6zu %10.3f %10.3f %10.3f %8.2f %7d %6zu\n",
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
					last.FrozenFrames,
					last.Routes
				);
			}
		}

		// **Every module's own report, and then the disagreements.** The report
		// is what each module says it did; the disagreements are the claims no
		// single module could have checked.
		std::printf("\n%s", unified::Format(crossing.Gather()).c_str());
		std::printf(
			"frozen frames    %llu of %llu (%.1f%%)\n",
			static_cast<unsigned long long>(frozen),
			static_cast<unsigned long long>(frames),
			frames == 0 ? 0.0 : 100.0 * static_cast<double>(frozen) / static_cast<double>(frames)
		);

		outcome.Contradictions = unified::CrossCheck(crossing.Gather()).size();
		return outcome;
	}

	// Fits a slope to every tag and answers whether any of them ran away.
	//
	// **The same check `mono.client` makes, and the second copy of it.** What
	// is shared is `HeapProfile::Runaway`, which does the fitting; what is
	// copied is the window arithmetic, the wording and the exit code - and
	// those are a program's own, because a soak script reads them. If a third
	// program wants this, that is the point at which it should move into
	// `Engine::core` rather than be pasted again.
	//
	// @param limit         Bytes per second a tag may climb at.
	// @param warmupSeconds Readings to leave out of the fit.
	// @return A process exit code.
	int CheckHeapGrowth(double limit, double warmupSeconds) {
		if (limit <= 0.0) {
			return 0;
		}
		if (!HeapProfile::IsCompiledIn()) {
			ENGINE_ERROR(
				"--heap-growth-limit was given and this build has no allocator hooks. Configure with "
				"MONO_HEAP_PROFILE=ON, the dev preset, or the -dev archive of this release."
			);
			return EXIT_RUNAWAY_HEAP;
		}

		const double retained = HeapProfile::HistorySeconds();
		const double window = retained - warmupSeconds;
		if (window < MINIMUM_GROWTH_WINDOW_SECONDS) {
			ENGINE_ERROR(
				"heap: {:.0f}s of readings less a {:.0f}s warm-up leaves nothing to fit a slope to. Run "
				"for longer than {:.0f}s.",
				retained,
				warmupSeconds,
				warmupSeconds + MINIMUM_GROWTH_WINDOW_SECONDS
			);
			return EXIT_RUNAWAY_HEAP;
		}

		const std::vector<engine::core::HeapGrowth> runaway = HeapProfile::Runaway(limit, window);
		if (runaway.empty()) {
			ENGINE_INFO("heap: steady over {:.0f}s - nothing climbing faster than {:.0f} B/s", window, limit);
			return 0;
		}

		for (const engine::core::HeapGrowth &entry : runaway) {
			ENGINE_ERROR(
				"heap: '{}' climbed {:.0f} B/s (fit {:.2f}) from {:.2f} MiB to {:.2f} MiB",
				entry.Path,
				entry.BytesPerSecond,
				entry.Fit,
				static_cast<double>(entry.FirstBytes) / (1024.0 * 1024.0),
				static_cast<double>(entry.LastBytes) / (1024.0 * 1024.0)
			);
		}
		ENGINE_ERROR("heap: {} tag(s) over the growth limit", runaway.size());
		return EXIT_RUNAWAY_HEAP;
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
		"unified_tests",
		"atomic - every arrangement of a client and a server in one process, and the reports they disagree "
		"about."
	);
	engine::core::Config::DeclareOptions(arguments);

	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag(
		"force-serial-compute",
		"Run every parallel dispatch on one thread, so the frame graph keeps every span"
	);
	arguments.Flag("quiet", "Print the reports only, not a line per tick");
	arguments.Flag("all", "Run every arrangement in turn");
	arguments.Flag("list-arrangements", "Print every arrangement's name, one per line, and exit");

	arguments.Value("arrangement", "NAME", "How to wire the halves: " + Arrangements());
	arguments.Value("entities", "N", "Entities in the placeholder world (default 64)");
	arguments.Value("scene", "PATH", "Author the server's world from a scene script");
	arguments.Value("ticks", "N", "Ticks to run after the join (default 120)");
	arguments.Value("seconds", "N", "Run each arrangement for this long instead of for a tick count");
	arguments.Value("tick-rate", "HZ", "The authority's tick rate (default 30)");
	arguments.Value("frames-per-tick", "N", "Frames drawn per tick (default 4)");
	arguments.Value("delay-ticks", "N", "The snapshot buffer's delay (default 2)");
	arguments.Value("drop", "N,N,...", "Message ordinals to lose silently, or arrival numbers when lossy");

	arguments.Value("heap-report", "PATH", "Write the heap profile to a file when the run ends");
	arguments.Value("heap-growth-limit", "BYTES", "Fail when a tag climbs faster than this, in bytes/second");
	arguments.Value("heap-warmup", "SECONDS", "Readings to leave out of the growth fit (default 10)");

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

	// **Before the engine's flags are applied, because it starts nothing.** A
	// script asking what the matrix contains should not have to bring a world
	// up to find out, and the list is what keeps a soak script in step with the
	// axes rather than holding a copy of them.
	if (arguments.Has("list-arrangements")) {
		for (const unified::Arrangement &arrangement : unified::AllArrangements()) {
			std::printf("%s\n", arrangement.Name().c_str());
		}
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
			std::fprintf(stderr, "--drop takes ordinals, comma separated: --drop 12,13\n");
			return 2;
		}
	}

	std::vector<unified::Arrangement> running;
	if (arguments.Has("all")) {
		running = unified::AllArrangements();
		if (arguments.Get("arrangement").has_value()) {
			std::fprintf(stderr, "--all and --arrangement ask for different runs; give one of them\n");
			return 2;
		}
	} else if (auto named = arguments.Get("arrangement")) {
		const std::optional<unified::Arrangement> parsedArrangement = unified::ParseArrangement(*named);
		if (!parsedArrangement.has_value()) {
			std::fprintf(stderr, "--arrangement takes one of: %s\n", Arrangements().c_str());
			return 2;
		}
		running.push_back(*parsedArrangement);
	} else {
		running.emplace_back();
	}

	const auto ticks = static_cast<uint64_t>(arguments.GetInteger("ticks", 120));
	const double seconds = arguments.GetNumber("seconds", 0.0);
	const bool quiet = arguments.Has("quiet") || running.size() > 1;

	const std::filesystem::path heapReport = arguments.Get("heap-report").has_value()
												 ? std::filesystem::path(*arguments.Get("heap-report"))
												 : std::filesystem::path();
	const double heapLimit = arguments.GetNumber("heap-growth-limit", 0.0);
	const double heapWarmup = arguments.GetNumber("heap-warmup", 10.0);
	const bool sampling = !heapReport.empty() || heapLimit > 0.0;

	if (sampling) {
		if (!HeapProfile::IsCompiledIn()) {
			ENGINE_WARN(
				"a heap option was given and this build has no allocator hooks. Configure with "
				"MONO_HEAP_PROFILE=ON, the dev preset, or the -dev archive of this release."
			);
		}
		HeapProfile::SetSamplingEnabled(true);
	}

	std::vector<Outcome> outcomes;
	outcomes.reserve(running.size());
	for (const unified::Arrangement &arrangement : running) {
		outcomes.push_back(RunOne(settings, arrangement, ticks, seconds, quiet, sampling));
	}

	// **The matrix, one line per arrangement.** With one arrangement this is a
	// restatement of what is above it; with twenty it is the whole answer, and
	// scrolling back through twenty reports to find the one that failed is
	// exactly what it exists to save.
	if (running.size() > 1) {
		std::printf("\n%-28s %8s %s\n", "arrangement", "joined", "contradictions");
		for (const Outcome &outcome : outcomes) {
			std::printf(
				"%-28s %8s %zu\n",
				outcome.Ran.Name().c_str(),
				outcome.Joined ? "yes" : "NO",
				outcome.Contradictions
			);
		}
	}

	if (!heapReport.empty()) {
		const engine::core::HeapTotals heap = HeapProfile::Totals();
		ENGINE_INFO(
			"heap: {:.1f} MiB live in {} block(s), {:.1f} MiB peak, {} tag(s)",
			static_cast<double>(heap.LiveBytes) / (1024.0 * 1024.0),
			heap.LiveBlocks,
			static_cast<double>(heap.PeakBytes) / (1024.0 * 1024.0),
			heap.Nodes
		);
		if (HeapProfile::WriteReport(heapReport)) {
			ENGINE_INFO("heap report written to {}", heapReport.string());
		} else {
			ENGINE_ERROR("could not write {}", heapReport.string());
		}
	}

	size_t failed = 0;
	for (const Outcome &outcome : outcomes) {
		failed += outcome.Joined && outcome.Contradictions == 0 ? 0 : 1;
	}
	if (failed > 0) {
		ENGINE_ERROR("{} of {} arrangement(s) failed", failed, outcomes.size());
		return 1;
	}

	return CheckHeapGrowth(heapLimit, heapWarmup);
}
