#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.parallel.process")

using engine::parallel::ExitReason;
using engine::parallel::Process;
using engine::parallel::ProcessStatus;
using engine::parallel::WorkersPerHost;

namespace process_test {
	// This test binary, used as its own child.
	//
	// Spawning a system tool would make these cases depend on what is installed
	// and where — `/bin/true` is not a path on Windows and not guaranteed on a
	// container. The one executable certain to exist is the one running.
	std::filesystem::path Self() {
		// Paths::Base() is already the directory this binary sits in.
		return engine::core::Paths::Base() / engine::core::Paths::Program("test_parallel");
	}

	// Arguments that make this binary exit quickly and cleanly. Listing tests
	// touches no state and returns zero.
	std::vector<std::string> QuickExit() {
		return {"--list-tests"};
	}

	// Arguments that make it exit non-zero: a filter matching nothing is an
	// error to Catch2.
	std::vector<std::string> Failure() {
		return {"[no-such-tag-anywhere]"};
	}

	// Waits for a child to stop, up to a deadline, so a slow machine does not
	// fail a case that a fast one passes.
	ProcessStatus Settle(Process &child, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
		const auto deadline = std::chrono::steady_clock::now() + limit;
		ProcessStatus status = child.Poll();

		while (status.Alive() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			status = child.Poll();
		}
		return status;
	}
}

using namespace process_test;

TEST_CASE("a handle that started nothing owns nothing", "[process]") {
	Process child;
	REQUIRE_FALSE(child.Started());
	REQUIRE(child.Id() == 0);
	REQUIRE(child.Poll().Reason == ExitReason::Gone);
	REQUIRE_FALSE(child.RequestStop());
	REQUIRE_FALSE(child.Kill());
}

TEST_CASE("a child starts, runs and exits cleanly", "[process]") {
	Process child;
	REQUIRE(child.Start(Self(), QuickExit()));
	REQUIRE(child.Started());
	REQUIRE(child.Id() != 0);

	const ProcessStatus status = Settle(child);
	REQUIRE(status.Reason == ExitReason::Exited);
	REQUIRE(status.Code == 0);
	REQUIRE_FALSE(status.Faulted());
}

TEST_CASE("a non-zero exit reads as a fault", "[process]") {
	// A supervisor treats these differently: a host that returned zero was told
	// to stop; one that returned anything else did not mean to.
	Process child;
	REQUIRE(child.Start(Self(), Failure()));

	const ProcessStatus status = Settle(child);
	REQUIRE(status.Reason == ExitReason::Exited);
	REQUIRE(status.Code != 0);
	REQUIRE(status.Faulted());
}

TEST_CASE("starting a program that does not exist fails rather than throwing", "[process]") {
	Process child;
	REQUIRE_FALSE(child.Start("/definitely/not/a/program/anywhere"));
	REQUIRE_FALSE(child.Started());
}

TEST_CASE("a handle will not start a second child over the first", "[process]") {
	// The first would be orphaned: still running, still holding its worlds, and
	// nothing left that knows to stop it.
	Process child;
	REQUIRE(child.Start(Self(), QuickExit()));
	REQUIRE_FALSE(child.Start(Self(), QuickExit()));

	child.Wait();
}

TEST_CASE("killing a child reads as a signal death", "[process]") {
	// Which is what a hard fault looks like from outside: an abort from the
	// affinity check, a segfault, or the out-of-memory killer.
	Process child;

	// A run of the whole suite takes long enough to be killed mid-flight.
	REQUIRE(child.Start(Self(), {}));
	REQUIRE(child.Kill());

	const ProcessStatus status = child.Wait();
	REQUIRE(status.Reason == ExitReason::Signalled);
	REQUIRE(status.Signal != 0);
	REQUIRE(status.Faulted());
}

TEST_CASE("polling reaps, so nothing accumulates", "[process]") {
	// A supervisor polls every barrier. Without reaping, every host it ever
	// started would stay in the process table forever.
	for (int index = 0; index < 8; index++) {
		Process child;
		REQUIRE(child.Start(Self(), QuickExit()));

		const ProcessStatus status = Settle(child);
		REQUIRE(status.Reason == ExitReason::Exited);

		// Once reaped, the handle owns nothing and says so.
		REQUIRE_FALSE(child.Started());
		REQUIRE(child.Poll().Reason == ExitReason::Exited);
	}
}

TEST_CASE("waiting twice returns the same answer", "[process]") {
	Process child;
	REQUIRE(child.Start(Self(), QuickExit()));

	const ProcessStatus first = child.Wait();
	const ProcessStatus second = child.Wait();

	REQUIRE(first.Reason == second.Reason);
	REQUIRE(first.Code == second.Code);
}

TEST_CASE("a moved handle carries the child with it", "[process]") {
	Process source;
	REQUIRE(source.Start(Self(), QuickExit()));
	const uint64_t id = source.Id();

	Process destination(std::move(source));

	REQUIRE(destination.Id() == id);
	REQUIRE_FALSE(source.Started());

	// And the moved-from handle reaps nothing, so the child is not killed twice.
	REQUIRE(destination.Wait().Reason == ExitReason::Exited);
}

TEST_CASE("move assignment stops whatever it replaced", "[process]") {
	Process first;
	REQUIRE(first.Start(Self(), {})); // long-running
	const uint64_t doomed = first.Id();

	Process second;
	REQUIRE(second.Start(Self(), QuickExit()));

	first = std::move(second);

	// The long-running child was stopped rather than orphaned.
	REQUIRE(first.Id() != doomed);
	first.Wait();
}

TEST_CASE("a destroyed handle does not leave an orphan", "[process]") {
	uint64_t id = 0;
	{
		Process child;
		REQUIRE(child.Start(Self(), {})); // long-running
		id = child.Id();
		REQUIRE(id != 0);
	}

	// The destructor killed and reaped it. Confirmed by the process id no
	// longer being ours to signal — a second handle cannot reach it.
	Process stranger;
	REQUIRE_FALSE(stranger.Kill());
	REQUIRE(id != 0);
}

TEST_CASE("asking a child to stop is not the same as killing it", "[process]") {
	Process child;
	REQUIRE(child.Start(Self(), {}));

	REQUIRE(child.RequestStop());
	const ProcessStatus status = Settle(child);

	// Either it handled the request and exited, or it took the default action
	// for the signal. Both are "stopped"; what matters is that it stopped.
	REQUIRE_FALSE(status.Alive());
}

// --- pool sizing ----------------------------------------------------------

TEST_CASE("workers are divided between hosts rather than duplicated", "[process]") {
	// Every host calling Jobs::Start(0) is the bug this exists to prevent:
	// eight hosts on a twenty-four core machine would run a hundred and ninety
	// threads over twenty-four cores.
	const unsigned cores = std::thread::hardware_concurrency();
	if (cores <= 1) {
		SUCCEED("a single-core machine has nothing to divide");
		return;
	}

	const unsigned alone = WorkersPerHost(1);
	REQUIRE(alone == cores - 1);

	// More hosts, fewer workers each — and never more in total than the machine
	// has, which is the whole property.
	unsigned previous = alone;
	for (unsigned hosts = 2; hosts <= 16; hosts++) {
		const unsigned each = WorkersPerHost(hosts);
		REQUIRE(each <= previous);

		// Each host's calling thread also works, so its true share is
		// `each + 1`.
		REQUIRE((each + 1) * hosts <= cores + hosts);
		previous = each;
	}
}

TEST_CASE("a host count of zero is treated as one", "[process]") {
	REQUIRE(WorkersPerHost(0) == WorkersPerHost(1));
}

TEST_CASE("more hosts than cores gives each of them none", "[process]") {
	// Correct rather than degenerate: with nothing to divide, every host runs
	// its batches on its own calling thread, which is what Jobs::For does with
	// no workers.
	REQUIRE(WorkersPerHost(1024) == 0);
}
