// The log, from outside the editor.
//
// **The editor could already be asked what it had said and no other program
// could.** `mono.studio` keeps an output panel, and the tool it registers reads
// that panel - so a dedicated server, which is the program most likely to be
// misbehaving unattended, answered nothing at all. This is the same question
// asked of the process rather than of a panel: a sink on the process-wide
// logger, a ring of the most recent lines, and a tool that reads it.
//
// **A ring rather than a file.** A log file is already written by whatever sink
// a program installed; what a model needs is the last few lines without a path,
// a permission and a tail. Bounded at `CAPACITY` for the reason every tool in
// this module is bounded: a reply nothing can read is not an answer, and the
// memory is charged to a program that asked for the surface.
//
// **The sink is installed by `AddDiagnosticTools` and not by the module.** A
// program that never opens a control port must not pay for a second sink on
// every line it writes, and the editor already has one of its own - which is
// why `mono.studio` registers its own `log_tail` over this one, reading the
// panel a person is looking at rather than a second ring beside it.

#include <engine/control/Surface.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace engine::control {

	using core::LogLevel;
	using nlohmann::json;

	namespace {
		// One line, as the ring keeps it.
		struct LogLine {
			LogLevel Level = LogLevel::Info;
			std::string Text;
		};

		// A level as the settings and `Log::LevelFromName` spell it.
		LogLevel LevelFrom(const std::string &text, bool &known) {
			LogLevel level = LogLevel::Info;
			known = core::LevelFromName(text, level);
			return level;
		}

		// spdlog's severity back to the engine's.
		//
		// **`critical` becomes `Error`**, because `Log::WriteUnfiltered` uses it
		// for an invariant that failed - a thing no log setting may hide, and a
		// thing a reader filtering to errors must still see.
		LogLevel LevelOf(spdlog::level::level_enum severity) {
			switch (severity) {
			case spdlog::level::trace:
				return LogLevel::Trace;
			case spdlog::level::debug:
				return LogLevel::Debug;
			case spdlog::level::info:
				return LogLevel::Info;
			case spdlog::level::warn:
				return LogLevel::Warning;
			default:
				return LogLevel::Error;
			}
		}

		// The most recent lines, and the sink that fills it.
		//
		// `base_sink<std::mutex>` gives the lock, which is what makes this safe
		// to write from a job worker and read from the thread `Answer` runs on.
		class RingSink final : public spdlog::sinks::base_sink<std::mutex> {
		  public:
			// Lines kept. Sized so that a fault and the hundred lines of
			// start-up before it both survive, and so that the buffer is tens of
			// kilobytes rather than megabytes.
			static constexpr size_t CAPACITY = 1024;

			// A copy of the most recent lines, oldest first.
			std::vector<LogLine> Recent() {
				std::lock_guard<std::mutex> held(mutex_);

				std::vector<LogLine> out;
				out.reserve(Lines.size());
				for (size_t step = 0; step < Lines.size(); step++) {
					out.push_back(Lines[(Oldest + step) % Lines.size()]);
				}
				return out;
			}

			// How many lines have been written since the sink was installed,
			// including the ones the ring has since dropped.
			uint64_t Written() {
				std::lock_guard<std::mutex> held(mutex_);
				return Total;
			}

		  protected:
			void sink_it_(const spdlog::details::log_msg &message) override {
				LogLine line{
					LevelOf(message.level),
					std::string(message.payload.data(), message.payload.size()),
				};

				Total++;
				if (Lines.size() < CAPACITY) {
					Lines.push_back(std::move(line));
					return;
				}

				// Full: overwrite the oldest and move the mark along, so the
				// buffer is allocated once and never grows.
				Lines[Oldest] = std::move(line);
				Oldest = (Oldest + 1) % CAPACITY;
			}

			void flush_() override {}

		  private:
			std::vector<LogLine> Lines;
			size_t Oldest = 0;
			uint64_t Total = 0;
		};

		// Installed once per process, however many surfaces ask for it.
		//
		// **Kept alive for the life of the process on purpose.** The logger is
		// process-wide and outlives any `Surface`; a sink detached while a job
		// worker was mid-line would be a use-after-free in the one component
		// whose whole job is to explain a crash.
		std::shared_ptr<RingSink> InstalledSink() {
			static std::shared_ptr<RingSink> sink = [] {
				auto created = std::make_shared<RingSink>();
				core::Log::Logger().sinks().push_back(created);
				return created;
			}();
			return sink;
		}

		// The `[category] ` prefix `Log::Write` puts on every line it formats.
		//
		// **Read back off the text rather than carried beside it**, because
		// spdlog's message carries the logger's name and not the engine's
		// category, and adding a second channel for it would mean changing
		// `core` to serve one tool. The format is one line of `Log.cpp` and this
		// comment is the pair of them.
		std::string CategoryOf(const std::string &line) {
			if (line.empty() || line.front() != '[') {
				return {};
			}
			const size_t close = line.find("] ");
			return close == std::string::npos ? std::string() : line.substr(1, close - 1);
		}
	}

	void Surface::AddDiagnosticTools() {
		const std::shared_ptr<RingSink> sink = InstalledSink();

		Add(Tool{
			"log_tail",
			"The most recent lines this program has logged, newest last. Filter by minimum level "
			"and by category - a category is one area of the engine, usually one module, and "
			"log_level lists the ones this build registers. Only the last 1024 lines are kept, and "
			"`dropped` says how many scrolled off before the oldest one shown.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"lines",
						  json{
							  {"type", "integer"},
							  {"description", "How many lines at most. Default 50, capped at 1024."},
						  }},
						 {"level",
						  json{
							  {"type", "string"},
							  {"description", "The lowest severity to include. Default trace."},
							  {"enum", json::array({"trace", "debug", "info", "warning", "error"})},
						  }},
						 {"category",
						  json{
							  {"type", "string"},
							  {"description", "Only lines from this area. Default all of them."},
						  }},
					 }},
				};
			},
			[sink](const json &arguments, std::string &failure) -> json {
				LogLevel floor = LogLevel::Trace;
				if (arguments.contains("level") && arguments["level"].is_string()) {
					bool known = false;
					floor = LevelFrom(arguments["level"].get<std::string>(), known);
					if (!known) {
						failure = "'" + arguments["level"].get<std::string>() +
								  "' is not a level. Use trace, debug, info, warning or error.";
						return nullptr;
					}
				}

				const auto wanted = static_cast<size_t>(std::clamp<int64_t>(
					arguments.value("lines", 50), 1, static_cast<int64_t>(RingSink::CAPACITY)
				));
				const std::string category = arguments.value("category", std::string());

				std::vector<LogLine> recent = sink->Recent();

				json lines = json::array();
				for (auto line = recent.rbegin(); line != recent.rend() && lines.size() < wanted; ++line) {
					if (line->Level < floor) {
						continue;
					}
					const std::string area = CategoryOf(line->Text);
					if (!category.empty() && area != category) {
						continue;
					}
					lines.push_back(
						json{
							{"level", core::Describe(line->Level)},
							{"category", area},
							{"text", line->Text},
						}
					);
				}

				std::reverse(lines.begin(), lines.end());

				return json{
					{"lines", std::move(lines)},
					{"written", sink->Written()},
					{"dropped", sink->Written() > recent.size() ? sink->Written() - recent.size() : 0},
					{"kept", recent.size()},
				};
			},
		});

		Add(Tool{
			"log_level",
			"Reads or changes what this program logs, while it runs. With no argument it lists "
			"every category the build has registered and the severity floor each is at. `set` takes "
			"the same text the engine.log-level setting does: a bare level for everything, or "
			"comma-separated category=level overrides, or both - `warning,net=trace`. Statements "
			"below the compiled floor were removed by the preprocessor and cannot be switched on.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"set",
						  json{
							  {"type", "string"},
							  {"description", "A level, or category=level terms, or both."},
						  }},
					 }},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				if (arguments.contains("set") && arguments["set"].is_string()) {
					const std::string specification = arguments["set"].get<std::string>();
					std::string_view unknown;
					if (!core::Log::Configure(specification, &unknown)) {
						// Nothing was applied, which is `Configure`'s own
						// contract: a half-applied setting is harder to notice
						// than one that plainly did not take.
						failure = "'" + std::string(unknown) + "' names no level, so nothing was changed.";
						return nullptr;
					}
				}

				json categories = json::array();
				for (uint32_t index = 0; index < core::Log::CategoryCount(); index++) {
					const core::LogCategory category = core::Log::CategoryAt(index);
					if (!category.IsValid()) {
						continue;
					}
					categories.push_back(
						json{
							{"category", std::string(category.Text())},
							{"level", core::Describe(core::Log::LevelOf(category.Text()))},
						}
					);
				}

				return json{
					{"default", core::Describe(core::Log::Level())},
					{"compiledFloor", core::Describe(core::Log::CompiledFloor())},
					{"categories", std::move(categories)},
				};
			},
		});

		Add(Tool{
			"metrics_read",
			"Every counter, gauge and distribution this process has recorded. A counter accumulates "
			"and is drained once a frame, so its value is a rate; a gauge is a level as last set; a "
			"histogram carries a mean, a maximum and nearest-rank percentiles over its recent "
			"window, which is what says whether a server holding its tick rate on average is still "
			"missing a fiftieth of its ticks. Times are nanoseconds. Reading changes nothing.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"prefix",
						  json{
							  {"type", "string"},
							  {"description", "Only names starting with this. Default all of them."},
						  }},
					 }},
				};
			},
			[](const json &arguments, std::string &) {
				const std::string prefix = arguments.value("prefix", std::string());
				const auto wanted = [&prefix](const core::Name &name) {
					return prefix.empty() || std::string(name.Text()).rfind(prefix, 0) == 0;
				};

				// **`Snapshot` rather than `Drain`.** Draining would hand this
				// call the frame's counters and leave whatever reads them per
				// frame with nothing - the sink's own comment calls that the two
				// readers fighting over each frame. Reporting reads; it does not
				// consume.
				const core::MetricsSnapshot taken = core::Metrics::Snapshot();

				json counters = json::array();
				for (const core::Counter &counter : taken.Counters) {
					if (!wanted(counter.Name)) {
						continue;
					}
					counters.push_back(
						json{
							{"name", std::string(counter.Name.Text())},
							{"value", counter.Value},
							{"samples", counter.Samples},
							{"time", counter.IsTime},
						}
					);
				}

				json gauges = json::array();
				for (const core::Gauge &gauge : taken.Gauges) {
					if (!wanted(gauge.Name)) {
						continue;
					}
					gauges.push_back(
						json{
							{"name", std::string(gauge.Name.Text())},
							{"value", gauge.Value},
							{"writes", gauge.Writes},
						}
					);
				}

				json histograms = json::array();
				for (const core::Histogram &histogram : taken.Histograms) {
					if (!wanted(histogram.Name)) {
						continue;
					}
					histograms.push_back(
						json{
							{"name", std::string(histogram.Name.Text())},
							{"samples", histogram.Samples},
							{"mean", histogram.Mean},
							{"minimum", histogram.Minimum},
							{"maximum", histogram.Maximum},
							{"p50", histogram.P50},
							{"p95", histogram.P95},
							{"p99", histogram.P99},
							{"retained", histogram.Retained},
							{"time", histogram.IsTime},
						}
					);
				}

				return json{
					{"counters", std::move(counters)},
					{"gauges", std::move(gauges)},
					{"histograms", std::move(histograms)},
				};
			},
		});
	}
}
