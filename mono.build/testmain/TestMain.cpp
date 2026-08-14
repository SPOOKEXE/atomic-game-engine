#include <engine/testing/Suite.hpp>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <catch2/reporters/catch_reporter_streaming_base.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

namespace {

	// The `mono` reporter: one tab-separated line per test case, then a totals
	// line. Selected with `--reporter mono::out=PATH`, which is what
	// mono.tools/testrunner passes alongside the console reporter so that a
	// person still reads the ordinary output and the runner still gets numbers.
	//
	//     case<TAB>pass|fail|skip<TAB>assertions passed<TAB>failed<TAB>microseconds<TAB>name
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
			m_stream << "# mono test report v2\n";
		}

		// Required by Catch::ReporterFactory, not optional as it is for a
		// listener.
		static std::string getDescription() {
			return "Tab-separated per-test-case results and timings, read by mono.tools/testrunner";
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
					 << OneLine(stats.testInfo->name) << '\n';

			Catch::StreamingReporterBase::testCaseEnded(stats);
		}

		// No duration on this line. The runner times a suite from the outside,
		// where starting the process is part of the cost, and one number with
		// two sources is one number that eventually disagrees with itself.
		void testRunEnded(const Catch::TestRunStats &stats) override {
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

CATCH_REGISTER_REPORTER("mono", MonoReporter)

int main(int argc, char **argv) {
	// Handled before Catch2 sees the command line, and it is the only argument
	// this main understands. Output is one suite per line:
	//
	//     engine.ecs.column.core<TAB>/abs/path/Column.cpp<TAB>engine.core.memory.arena
	for (int index = 1; index < argc; index++) {
		if (std::strcmp(argv[index], "--mono-suites") != 0) {
			continue;
		}

		for (const auto &suite : engine::testing::Registry::All()) {
			std::cout << suite.Id << '\t' << suite.File << '\t';
			for (size_t depth = 0; depth < suite.Depends.size(); depth++) {
				if (depth > 0) {
					std::cout << ',';
				}
				std::cout << suite.Depends[depth];
			}
			std::cout << '\n';
		}
		return 0;
	}

	return Catch::Session().run(argc, argv);
}
