#include <engine/core/Paths.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <testrunner/Process.hpp>
#include <testrunner/Runner.hpp>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_ID("tools.testrunner.process")

using testrunner::Run;

namespace {
	namespace fs = std::filesystem;
	using namespace std::chrono_literals;

	constexpr const char *SCRATCH_VARIABLE = "MONO_TESTRUNNER_PROCESS_SCRATCH";
	const std::string &Self() {
		static const std::string path =
			(engine::core::Paths::Base() / engine::core::Paths::Program("test_testrunner")).string();
		return path;
	}

	class ScratchEnvironment {
	  public:
		explicit ScratchEnvironment(const std::string &path) {
			if (const char *previous = std::getenv(SCRATCH_VARIABLE)) {
				Previous = previous;
			}
#ifdef _WIN32
			Ready = _putenv_s(SCRATCH_VARIABLE, path.c_str()) == 0;
#else
			Ready = setenv(SCRATCH_VARIABLE, path.c_str(), 1) == 0;
#endif
		}

		~ScratchEnvironment() {
			if (!Ready) return;
#ifdef _WIN32
			_putenv_s(SCRATCH_VARIABLE, Previous ? Previous->c_str() : "");
#else
			if (Previous) {
				setenv(SCRATCH_VARIABLE, Previous->c_str(), 1);
			} else {
				unsetenv(SCRATCH_VARIABLE);
			}
#endif
		}

		bool Ready = false;

	  private:
		std::optional<std::string> Previous;
	};

	bool WaitForReadyChildren(const fs::path &directory, size_t expected) {
		const auto deadline = std::chrono::steady_clock::now() + 8s;
		while (std::chrono::steady_clock::now() < deadline) {
			size_t ready = 0;
			for (const auto &entry : fs::directory_iterator(directory)) {
				if (entry.is_directory() && entry.path().filename().string().starts_with("ready-")) {
					ready++;
				}
			}
			if (ready == expected) return true;
			std::this_thread::sleep_for(10ms);
		}
		return false;
	}
}

TEST_CASE("process inheritance held child", "[.process-helper]") {
	const char *scratch = std::getenv(SCRATCH_VARIABLE);
	REQUIRE(scratch != nullptr);
	const fs::path directory(scratch);

	bool marked = false;
	for (size_t index = 0; index < 256; index++) {
		if (fs::create_directory(directory / ("ready-" + std::to_string(index)))) {
			marked = true;
			break;
		}
	}
	REQUIRE(marked);

	const auto deadline = std::chrono::steady_clock::now() + 20s;
	while (!fs::exists(directory / "release") && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(10ms);
	}
	REQUIRE(fs::exists(directory / "release"));
}

TEST_CASE("process inheritance large output child", "[.process-helper]") {
	const std::string payload(256 * 1024, 'X');
	REQUIRE(std::fwrite(payload.data(), 1, payload.size(), stdout) == payload.size());
	REQUIRE(std::fflush(stdout) == 0);
}

TEST_CASE("a program that does not exist did not start", "[process]") {
	const auto result = Run({"/nonexistent/definitely-not-a-program"});

	// Distinct from a non-zero exit, which is the program running and
	// disagreeing with you. Conflating the two turns a typo in a path into
	// "your tests failed".
	REQUIRE_FALSE(result.Started);
}

TEST_CASE("an empty argument list does nothing", "[process]") {
	REQUIRE_FALSE(Run({}).Started);
}

TEST_CASE("this test binary can list its own suites", "[process]") {
	// The runner's discovery path, exercised against the one binary that is
	// guaranteed to be present and executable while this test is running.
	const auto self = Run({Self(), "--mono-suites"});

	REQUIRE(self.Started);
	REQUIRE(self.ExitCode == 0);
	REQUIRE(self.Output.find("tools.testrunner.process") != std::string::npos);
	// Tab-separated: id, source, dependencies.
	REQUIRE(self.Output.find('\t') != std::string::npos);
}

TEST_CASE("a non-zero exit is reported without losing the output", "[process]") {
	const auto result = Run({Self(), "--this-is-not-a-catch2-option"});

	REQUIRE(result.ExitCode != 0);
	// Output has to survive a failure, or a failing suite reports nothing
	// useful - which is precisely when the output is wanted.
	REQUIRE_FALSE(result.Output.empty());
}

TEST_CASE("output larger than a pipe buffer does not deadlock", "[process]") {
	// Waiting for the child before draining the pipe deadlocks as soon as it
	// writes more than 64 KiB, which for a verbose test run is immediately.
	// The child writes a known 256 KiB payload, larger than a pipe buffer.
	const auto result = Run({Self(), "process inheritance large output child"});

	const std::string payload(256 * 1024, 'X');
	REQUIRE(result.Started);
	REQUIRE(result.ExitCode == 0);
	REQUIRE(result.Output.find(payload) != std::string::npos);
}

TEST_CASE("concurrent children do not hold one another's output open", "[process]") {
	constexpr size_t childCount = 48;
	constexpr size_t heldCount = 12;
	const std::string &self = Self();
	const testrunner::ReportScratchDirectory scratch(fs::temp_directory_path());
	REQUIRE_FALSE(scratch.Path().empty());
	const ScratchEnvironment environment(scratch.Path().string());
	REQUIRE(environment.Ready);

	std::mutex launchMutex;
	std::condition_variable launchSignal;
	size_t waiting = 0;
	bool launch = false;
	std::vector<std::future<testrunner::ProcessResult>> children;
	children.reserve(childCount);
	std::vector<std::thread> workers;
	workers.reserve(childCount);
	// On partial startup or an exception, release and join every started worker.
	struct ReleaseOnExit {
		std::mutex &LaunchMutex;
		std::condition_variable &LaunchSignal;
		bool &Launch;
		fs::path Path;
		std::vector<std::thread> &Workers;
		~ReleaseOnExit() {
			{
				std::lock_guard lock(LaunchMutex);
				Launch = true;
			}
			LaunchSignal.notify_all();
			std::ofstream(Path / "release").put('\n');
			for (auto &worker : Workers) {
				if (worker.joinable()) worker.join();
			}
		}
	} release{launchMutex, launchSignal, launch, scratch.Path(), workers};

	// Dedicated threads let every worker reach the barrier; pooled tasks could
	// remain queued behind workers already waiting there.
	for (size_t index = 0; index < childCount; index++) {
		std::packaged_task<testrunner::ProcessResult()> child([&, index] {
			{
				std::unique_lock lock(launchMutex);
				waiting++;
				launchSignal.notify_all();
				launchSignal.wait(lock, [&] { return launch; });
			}
			if (index % 4 == 0) return Run({self, "process inheritance held child"});
			return Run({self, "--mono-suites"});
		});
		children.push_back(child.get_future());
		workers.emplace_back(std::move(child));
	}
	const auto launchDeadline = std::chrono::steady_clock::now() + 8s;
	std::unique_lock launchLock(launchMutex);
	launchSignal.wait_until(launchLock, launchDeadline, [&] { return waiting == childCount; });
	const size_t workersReadyAtDeadline = waiting;
	const bool allWorkersReady = workersReadyAtDeadline == childCount;
	launch = true;
	launchLock.unlock();
	launchSignal.notify_all();

	const bool allHeldStarted = allWorkersReady && WaitForReadyChildren(scratch.Path(), heldCount);
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	bool shortChildrenCompleted = allHeldStarted;
	for (size_t index = 0; index < childCount; index++) {
		if (index % 4 != 0 && children[index].wait_until(deadline) != std::future_status::ready) {
			shortChildrenCompleted = false;
		}
	}
	bool heldChildrenStillRunning = allHeldStarted;
	for (size_t index = 0; index < childCount; index++) {
		if (index % 4 == 0 && children[index].wait_for(0s) == std::future_status::ready) {
			heldChildrenStillRunning = false;
		}
	}

	// Release and join before any assertion can leave a child behind.
	std::ofstream(scratch.Path() / "release").put('\n');
	std::vector<testrunner::ProcessResult> outcomes;
	outcomes.reserve(childCount);
	for (auto &child : children)
		outcomes.push_back(child.get());
	for (auto &worker : workers)
		worker.join();

	INFO("workers ready at deadline: " << workersReadyAtDeadline << '/' << childCount);
	REQUIRE(allWorkersReady);
	REQUIRE(allHeldStarted);
	REQUIRE(heldChildrenStillRunning);
	REQUIRE(shortChildrenCompleted);
	for (size_t index = 0; index < outcomes.size(); index++) {
		INFO("child " << index << ": " << outcomes[index].Output);
		CHECK(outcomes[index].Started);
		CHECK(outcomes[index].ExitCode == 0);
		if (index % 4 != 0)
			CHECK(outcomes[index].Output.find("tools.testrunner.process") != std::string::npos);
	}
}
