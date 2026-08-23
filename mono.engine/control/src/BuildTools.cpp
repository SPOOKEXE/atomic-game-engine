// Running the suites, without holding the frame that asked for them.
//
// **This is the tool that could not be written the obvious way.** `just
// test-all` is 171 seconds of suite time on this tree and `just test` is
// whatever the cascade selects; a tool that ran either one synchronously would
// hold `Surface::Answer`, which holds `Server::Pump`, which holds the frame or
// the tick - and `AGENTS.md` says in those words that a tool needing slow work
// starts it and returns a handle. So `test_run` spawns and returns, and
// `test_result` is the handle.
//
// ## What a tool is allowed to invoke
//
// **Exactly one program, named by this file and not by the caller.** The child
// is `<build>/tools/testrunner`, found from the build directory this executable
// was staged into, with an argument list assembled here. There is no shell, no
// `just`, and no path a client can influence: the only thing `test_run` takes
// from its arguments is two booleans. That matters more than it looks - the
// surface has no authentication, and a tool that took a command line would turn
// "can reach loopback" into "can run anything", which is a different boundary
// from the one `SECURITY.md` describes.
//
// **It runs the suites and does not build them.** `just test` builds first; this
// deliberately does not, because a compile is minutes of a machine's whole CPU
// and the caller did not ask for one. A tree that has not been built runs the
// binaries that are there, which is the same thing `./testrunner` on its own
// does, and the report says which.
//
// **One run at a time, per process.** A second `test_run` while one is in flight
// is refused with the handle of the one already going, rather than starting a
// second runner that would fight the first for the cascade cache.
//
// The cache is the same `.cache/smart-tests.txt` `just test` uses, on purpose: a
// run started from here counts as a run, and a suite it turned green is a suite
// the next `just test` may skip.

#include "Report.hpp"

#include <engine/control/Repository.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace engine::control {

	using nlohmann::json;

	namespace {
		// The run in flight, or the last one that finished.
		//
		// Process-wide rather than per-surface, because the thing it guards is
		// process-wide: two surfaces in one program would still be one cascade
		// cache and one report directory.
		struct TestRun {
			parallel::Process Child;
			std::filesystem::path Report;
			std::vector<std::string> Command;
			uint64_t Handle = 0;
			uint64_t StartedNanoseconds = 0;
			double Seconds = 0.0;
			parallel::ProcessStatus Ended;
			bool Running = false;
			bool Finished = false;
		};

		// **A function-local static, so `~Process` runs at exit.** A run still
		// going when the program stops is terminated and reaped rather than
		// orphaned - an editor somebody closed must not leave a test runner
		// behind holding the cascade cache.
		TestRun &Current() {
			static TestRun run;
			return run;
		}

		// Latches the child's exit the first time it is seen.
		//
		// **`Process::Poll` reaps**, so the status is available exactly once;
		// asking a second time reports `Gone`, which is not what happened.
		void Refresh(TestRun &run) {
			if (!run.Running) {
				return;
			}

			const parallel::ProcessStatus status = run.Child.Poll();
			if (status.Alive()) {
				return;
			}

			run.Ended = status;
			run.Running = false;
			run.Finished = true;
			run.Seconds =
				static_cast<double>(core::Clock::Nanoseconds() - run.StartedNanoseconds) / 1'000'000'000.0;
		}

		std::string Read(const std::filesystem::path &path) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return {};
			}
			std::ostringstream contents;
			contents << file.rdbuf();
			return contents.str();
		}
	}

	// The report's opening paragraphs, which are the whole answer when
	// everything passed.
	std::string Summary(const std::string &report) {
		std::istringstream lines(report);
		std::string line;
		std::string out;
		size_t kept = 0;

		while (std::getline(lines, line) && kept < 3) {
			// **The section heading ends it, and the check comes before the
			// one that skips a heading.** With the two the other way round,
			// `## cdn` was skipped rather than stopping the walk and the
			// summary carried a per-section line that reads like a total.
			if (line.rfind("## ", 0) == 0 || (!line.empty() && line.front() == '|')) {
				break;
			}
			if (line.empty() || line.front() == '#') {
				continue;
			}
			out += line;
			out += '\n';
			kept++;
		}

		return out;
	}

	// Every suite whose row says `fail`.
	//
	// The report's rows are `| \`suite.id\` | ... | FAIL |`, which is one
	// line of `mono.tools/testrunner`'s writer and this is the pair of it. A
	// reader that got this wrong would report a green run, so the suite
	// covers a failing report as well as a passing one - the first draft
	// looked for a lowercase `fail` and reported no failed suites over a
	// report with five.
	std::vector<std::string> Failures(const std::string &report) {
		std::istringstream lines(report);
		std::string line;
		std::vector<std::string> failed;

		while (std::getline(lines, line)) {
			if (line.rfind("| `", 0) != 0 || line.find("| FAIL |") == std::string::npos) {
				continue;
			}
			const size_t close = line.find('`', 3);
			if (close != std::string::npos) {
				failed.push_back(line.substr(3, close - 3));
			}
		}

		return failed;
	}

	void Surface::AddBuildTools() {
		Add(Tool{
			"test_run",
			"Starts this repository's test suites and returns immediately. `all` runs every suite, "
			"which took 171 seconds of suite time when this was written; the default runs only the "
			"suites a change could have affected, by the same cascading signature `just test` uses. "
			"It runs the binaries that are built and does not build them. Poll test_result for the "
			"outcome - nothing here blocks, because a tool runs inside the program's frame.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"all",
						  json{
							  {"type", "boolean"},
							  {"description", "Run every suite rather than the affected ones."},
						  }},
						 {"verbose",
						  json{{"type", "boolean"}, {"description", "Name every suite that was skipped."}}},
					 }},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				TestRun &run = Current();
				Refresh(run);

				if (run.Running) {
					failure = "a test run started " + std::to_string(run.Handle) +
							  " is still going. Call test_result, or wait for it to finish.";
					return nullptr;
				}

				const std::filesystem::path &build = BuildDirectory();
				const std::filesystem::path &root = RepositoryRoot();
				if (build.empty()) {
					failure = "test_run needs the build tree this program was staged into, and this "
							  "executable is not inside one.";
					return nullptr;
				}

				const std::filesystem::path runner = build / "tools" / core::Paths::Program("testrunner");
				std::error_code problem;
				if (!std::filesystem::exists(runner, problem)) {
					failure = "testrunner is not built. Run `just build testrunner` and try again.";
					return nullptr;
				}

				run.Report = build / "control-tests";
				std::filesystem::create_directories(run.Report, problem);

				// Removed before the run, so a report left by the previous one
				// cannot be read back as this one's result.
				std::filesystem::remove(run.Report / "test-output.md", problem);

				run.Command = {
					runner.string(),
					"--build",
					build.string(),
					"--report",
					run.Report.string(),
				};
				if (!root.empty()) {
					run.Command.push_back("--cache");
					run.Command.push_back((root / ".cache" / "smart-tests.txt").string());
				}
				if (arguments.value("all", false)) {
					run.Command.emplace_back("--all");
				}
				if (arguments.value("verbose", false)) {
					run.Command.emplace_back("--verbose");
				}

				const std::vector<std::string> tail(run.Command.begin() + 1, run.Command.end());
				if (!run.Child.Start(runner, tail)) {
					failure = "could not start " + runner.string();
					return nullptr;
				}

				run.Handle = run.Child.Id();
				run.StartedNanoseconds = core::Clock::Nanoseconds();
				run.Running = true;
				run.Finished = false;

				json command = json::array();
				for (const std::string &word : run.Command) {
					command.push_back(word);
				}

				return json{
					{"state", "running"},
					{"handle", run.Handle},
					{"command", std::move(command)},
					{"report", (run.Report / "test-output.md").string()},
				};
			},
		});

		Add(Tool{
			"test_result",
			"How the run test_run started is getting on. While it runs, that is all this says. When "
			"it has finished it carries the summary, every suite that failed, and how long it took; "
			"pass `full` for the whole generated report, which is tens of kilobytes of per-suite "
			"tables. A non-zero exit with no failed suites means the runner itself refused - read "
			"`summary`.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"full",
						  json{
							  {"type", "boolean"},
							  {"description", "Include the whole report rather than the summary."},
						  }},
					 }},
				};
			},
			[](const json &arguments, std::string &failure) -> json {
				TestRun &run = Current();
				Refresh(run);

				if (!run.Running && !run.Finished) {
					failure = "nothing has been run. Call test_run first.";
					return nullptr;
				}

				if (run.Running) {
					return json{
						{"state", "running"},
						{"handle", run.Handle},
						{"seconds",
						 static_cast<double>(core::Clock::Nanoseconds() - run.StartedNanoseconds) /
							 1'000'000'000.0},
					};
				}

				const std::string report = Read(run.Report / "test-output.md");
				const std::vector<std::string> failed = Failures(report);

				json names = json::array();
				for (const std::string &name : failed) {
					names.push_back(name);
				}

				json out{
					{"state", "finished"},
					{"handle", run.Handle},
					{"exitCode", run.Ended.Code},
					{"passed", run.Ended.Reason == parallel::ExitReason::Exited && run.Ended.Code == 0},
					{"seconds", run.Seconds},
					{"summary", Summary(report)},
					{"failed", std::move(names)},
					{"report", (run.Report / "test-output.md").string()},
				};

				if (run.Ended.Reason == parallel::ExitReason::Signalled) {
					// A runner that took a signal did not write a report, and
					// saying "no suites failed" about that would be a lie by
					// omission.
					out["signal"] = run.Ended.Signal;
				}

				if (arguments.value("full", false)) {
					out["full"] = report;
				}

				return out;
			},
		});
	}
}
