#include "SelectedCases.hpp"
#include "SuiteDiscovery.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_spec.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/interfaces/catch_interfaces_testcase.hpp>
#include <catch2/internal/catch_sharding.hpp>
#include <catch2/internal/catch_test_case_registry_impl.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <catch2/reporters/catch_reporter_streaming_base.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace {

	std::set<const Catch::TestCaseHandle *> SelectCases(const Catch::IConfig &config) {
		// Match Catch::TestGroup's pointer-set ordering before sharding. A
		// separately sorted list would choose different cases for each shard.
		const auto &all = Catch::getAllTestCasesSorted(config);
		std::set<const Catch::TestCaseHandle *> selected;
		if (config.testSpec().hasFilters()) {
			for (const auto &match : config.testSpec().matchesByFilter(all, config)) {
				selected.insert(match.tests.begin(), match.tests.end());
			}
		} else {
			for (const auto &test : all) {
				if (!test.getTestCaseInfo().isHidden()) selected.insert(&test);
			}
		}
		return Catch::createShard(selected, config.shardCount(), config.shardIndex());
	}

	class SelectionListener : public Catch::EventListenerBase {
	  public:
		using Catch::EventListenerBase::EventListenerBase;
		static std::string getDescription() {
			return "Freeze exact selected cases for shared scenarios";
		}
		void testRunStarting(const Catch::TestRunInfo &) override {
			std::vector<engine::testing::SelectedCase> selected;
			for (const auto *test : SelectCases(*m_config)) {
				const auto &info = test->getTestCaseInfo();
				selected.push_back({info.lineInfo.file, info.name});
			}
			engine::testing::detail::FreezeSelectedCases(std::move(selected));
		}
	};

	// The `mono` reporter: one tab-separated line per test case, then a totals
	// line. Selected with `--reporter mono::out=PATH`, which is what
	// mono.tools/testrunner passes alongside the console reporter so that a
	// person still reads the ordinary output and the runner still gets numbers.
	//
	//     failure-exit<TAB>framework assertion-failure status
	//     source<TAB>eligible cases<TAB>source path
	//     manifest-end
	//     case<TAB>pass|fail|skip<TAB>assertions passed<TAB>failed<TAB>microseconds<TAB>source<TAB>name
	//     total<TAB>cases passed<TAB>failed<TAB>skipped<TAB>assertions passed<TAB>failed
	//
	// Catch2 already emits XML and JSON, and either would mean carrying a parser
	// in the runner for a format an order of magnitude wider than the seven
	// numbers wanted. This is the same tab-separated shape as `--mono-suites`
	// and `smart-tests.txt`, so the runner reads it with the splitter it has.
	class MonoReporter : public Catch::StreamingReporterBase {
	  public:
		explicit MonoReporter(Catch::ReporterConfig &&config)
			: Catch::StreamingReporterBase(std::move(config)) {
			// mono.tools/testrunner refuses a report whose header it does not
			// know, for the same reason it discards a cache from another
			// version: numbers that were gathered differently do not mean the
			// same thing. Change one side of this and change the other.
			m_stream << "# mono test report v3\n";
		}

		// Required by Catch::ReporterFactory, not optional as it is for a
		// listener.
		static std::string getDescription() {
			return "Tab-separated per-test-case results and timings, read by mono.tools/testrunner";
		}

		// Declare the filtered workload before any case runs. Totals alone cannot
		// prove that an eligible source was not silently omitted from a group.
		void testRunStarting(const Catch::TestRunInfo &info) override {
			Catch::StreamingReporterBase::testRunStarting(info);
			m_stream << "failure-exit\t" << Catch::TestFailureExitCode << '\n';
			std::map<std::string, size_t> sources;
			for (const auto &suite : engine::testing::Registry::All())
				sources.try_emplace(OneLine(std::string(suite.File)), 0);
			for (const auto *test : SelectCases(*m_config)) {
				sources[OneLine(test->getTestCaseInfo().lineInfo.file)]++;
			}
			for (const auto &[source, count] : sources)
				m_stream << "source\t" << count << '\t' << source << '\n';
			m_stream << "manifest-end\n";
		}

		// A steady_clock spanning start to end rather than the root section's
		// reported duration, because a case holding SECTIONs is entered once per
		// leaf and the question the report answers is what the whole case cost.
		//
		// Steady, not system: the report is a difference between two readings,
		// and a clock somebody may set backwards mid-run is the one clock that
		// cannot be subtracted from itself.
		void testCaseStarting(const Catch::TestCaseInfo &info) override {
			m_started = std::chrono::steady_clock::now();
			Catch::StreamingReporterBase::testCaseStarting(info);
		}

		// The stats handed to this one are the delta for the case that just
		// ended, not the run so far, so no subtraction is needed here.
		void testCaseEnded(const Catch::TestCaseStats &stats) override {
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - m_started
			);

			const auto &cases = stats.totals.testCases;
			const char *outcome = cases.failed > 0 ? "fail" : (cases.skipped > 0 ? "skip" : "pass");

			// Microseconds, as an integer. A decimal here would put the report's
			// numbers at the mercy of whatever locale the binary started in -
			// half of Europe writes 1,5 and the runner splits on tabs, not
			// commas, so it would parse and be wrong by a factor of ten.
			m_stream << "case\t" << outcome << '\t' << stats.totals.assertions.passed << '\t'
					 << stats.totals.assertions.failed << '\t' << elapsed.count() << '\t'
					 << OneLine(stats.testInfo->lineInfo.file) << '\t' << OneLine(stats.testInfo->name)
					 << '\n';

			Catch::StreamingReporterBase::testCaseEnded(stats);
		}

		// No duration on this line. The runner times a suite from the outside,
		// where starting the process is part of the cost, and one number with
		// two sources is one number that eventually disagrees with itself.
		void testRunEnded(const Catch::TestRunStats &stats) override {
			if (stats.aborting) {
				m_stream.flush();
				Catch::StreamingReporterBase::testRunEnded(stats);
				return;
			}
			m_stream << "total\t" << stats.totals.testCases.passed << '\t' << stats.totals.testCases.failed
					 << '\t' << stats.totals.testCases.skipped << '\t' << stats.totals.assertions.passed
					 << '\t' << stats.totals.assertions.failed << '\n';
			m_stream.flush();

			Catch::StreamingReporterBase::testRunEnded(stats);
		}

	  private:
		std::chrono::steady_clock::time_point m_started;

		// A record is one line and its free-text field is last, so a test name
		// carrying a tab or a newline would otherwise end the record early. A
		// name is prose written by a person; it is not required to be safe.
		static std::string OneLine(const std::string &text) {
			std::string flattened = text;
			for (char &character : flattened) {
				if (character == '\t' || character == '\n' || character == '\r') {
					character = ' ';
				}
			}
			return flattened;
		}
	};
}

CATCH_REGISTER_LISTENER(SelectionListener)
CATCH_REGISTER_REPORTER("mono", MonoReporter)

int main(int argc, char **argv) {
	// Handled before Catch2 sees the command line, and it is the only argument
	// this main understands. Output is one suite per line: id, source,
	// comma-separated dependencies, tier and scenario group (both optional):
	//
	//     engine.ecs.column.core<TAB>/abs/path/Column.cpp<TAB>engine.core.memory.arena<TAB>fast
	for (int index = 1; index < argc; index++) {
		if (std::strcmp(argv[index], "--mono-suites") != 0) {
			continue;
		}

		const auto missing = engine::testing::detail::MissingSuiteSources(
			Catch::getRegistryHub().getTestCaseRegistry().getAllTests(), engine::testing::Registry::All()
		);
		if (!missing.empty()) {
			for (const auto source : missing)
				std::cerr << "test source has no TEST_SUITE_ID: " << source << '\n';
			return 1;
		}

		for (const auto &suite : engine::testing::Registry::All()) {
			std::cout << suite.Id << '\t' << suite.File << '\t';
			for (size_t depth = 0; depth < suite.Depends.size(); depth++) {
				if (depth > 0) {
					std::cout << ',';
				}
				std::cout << suite.Depends[depth];
			}
			std::cout << '\t' << suite.Tier << '\t' << suite.ScenarioGroup << '\n';
		}
		return 0;
	}

	return Catch::Session().run(argc, argv);
}
