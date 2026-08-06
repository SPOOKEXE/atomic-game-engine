#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.parallel.jobs")

using engine::parallel::Jobs;

namespace {
	struct Pool {
		explicit Pool(unsigned workers) {
			Jobs::Start(workers);
		}
		~Pool() {
			Jobs::Stop();
		}
	};
}

TEST_CASE("every index is visited exactly once", "[jobs]") {
	Pool pool{4};

	constexpr size_t COUNT = 10'000;
	std::vector<std::atomic<int>> visits(COUNT);

	Jobs::For(COUNT, 64, [&](size_t begin, size_t end) {
		for (size_t index = begin; index < end; index++) {
			visits[index].fetch_add(1, std::memory_order_relaxed);
		}
	});

	for (size_t index = 0; index < COUNT; index++) {
		REQUIRE(visits[index].load() == 1);
	}
}

TEST_CASE("ranges never overlap", "[jobs]") {
	Pool pool{4};

	std::mutex guard;
	std::vector<std::pair<size_t, size_t>> ranges;

	Jobs::For(1000, 100, [&](size_t begin, size_t end) {
		std::lock_guard lock(guard);
		ranges.emplace_back(begin, end);
	});

	std::sort(ranges.begin(), ranges.end());
	for (size_t index = 1; index < ranges.size(); index++) {
		REQUIRE(ranges[index].first >= ranges[index - 1].second);
	}
	REQUIRE(ranges.front().first == 0);
	REQUIRE(ranges.back().second == 1000);
}

TEST_CASE("a small span runs inline rather than paying for a handover", "[jobs]") {
	Pool pool{4};

	std::set<std::thread::id> threads;
	std::mutex guard;

	Jobs::For(10, 256, [&](size_t, size_t) {
		std::lock_guard lock(guard);
		threads.insert(std::this_thread::get_id());
	});

	REQUIRE(threads.size() == 1);
	REQUIRE(*threads.begin() == std::this_thread::get_id());
}

TEST_CASE("work actually reaches more than one thread", "[jobs]") {
	Pool pool{4};

	// Retried, because one dispatch is legitimately allowed to stay on one
	// thread: the calling thread drains ranges too, and on a loaded machine it
	// can finish the whole batch before a worker is scheduled. Asserting on a
	// single dispatch made this fail about once in a hundred runs, always
	// while the machine was busy with something else.
	//
	// Failing every attempt means the pool is not participating at all, which
	// is the thing worth catching.
	constexpr int ATTEMPTS = 25;
	size_t widest = 0;

	for (int attempt = 0; attempt < ATTEMPTS && widest <= 1; attempt++) {
		std::set<std::thread::id> threads;
		std::mutex guard;

		Jobs::For(100'000, 128, [&](size_t begin, size_t end) {
			volatile size_t sink = 0;
			for (size_t index = begin; index < end; index++) {
				sink += index;
			}
			std::lock_guard lock(guard);
			threads.insert(std::this_thread::get_id());
		});

		widest = std::max(widest, threads.size());
	}

	REQUIRE(widest > 1);
}

TEST_CASE("For with no pool still runs everything", "[jobs]") {
	// No Start(). Every engine path has to work single-threaded, because a
	// unit test and the asset cooker both run without a pool.
	std::atomic<size_t> total{0};

	Jobs::For(500, 16, [&](size_t begin, size_t end) {
		total.fetch_add(end - begin, std::memory_order_relaxed);
	});

	REQUIRE(total.load() == 500);
}

TEST_CASE("a count of zero does nothing", "[jobs]") {
	Pool pool{2};

	bool called = false;
	Jobs::For(0, 8, [&](size_t, size_t) { called = true; });

	REQUIRE_FALSE(called);
}

TEST_CASE("an exception in a range reaches the caller", "[jobs]") {
	Pool pool{4};

	// An exception escaping a worker would terminate the process. It has to
	// come back out of For on the calling thread instead.
	REQUIRE_THROWS_AS(
		Jobs::For(
			10'000,
			64,
			[](size_t begin, size_t) {
				if (begin == 0) {
					throw std::runtime_error("range failed");
				}
			}
		),
		std::runtime_error
	);
}

// --- one batch at a time -------------------------------------------------
//
// A second dispatch finding the pool occupied runs its span inline rather than
// waiting or corrupting the batch already in flight. These cover the two ways
// that happens — nested, and from another thread — because both become the
// ordinary case once a world tick is itself a range in a larger batch.

namespace {
	// Counts entries that were not visited exactly once.
	//
	// A reduction rather than an assertion per index: a REQUIRE per element
	// turns a completeness check over tens of thousands of rows into tens of
	// thousands of recorded assertions, and the failure message is no better
	// for it.
	size_t NotVisitedExactlyOnce(const std::vector<std::atomic<int>> &visits) {
		size_t wrong = 0;
		for (const auto &visit : visits) {
			if (visit.load(std::memory_order_relaxed) != 1) {
				wrong++;
			}
		}
		return wrong;
	}

	// Whether a dispatch of `count` reached a thread other than the caller.
	//
	// Retried for the reason the wide-dispatch case above is: one dispatch is
	// allowed to stay on the calling thread, because the caller drains too.
	bool PoolParticipates(size_t count) {
		constexpr int ATTEMPTS = 25;

		for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
			std::atomic<bool> elsewhere{false};
			const std::thread::id caller = std::this_thread::get_id();

			Jobs::For(count, 128, [&](size_t begin, size_t end) {
				if (std::this_thread::get_id() != caller) {
					elsewhere.store(true, std::memory_order_relaxed);
				}
				volatile size_t sink = 0;
				for (size_t index = begin; index < end; index++) {
					sink += index;
				}
			});

			if (elsewhere.load(std::memory_order_relaxed)) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("a nested For runs inline rather than deadlocking", "[jobs]") {
	Pool pool{4};

	constexpr size_t OUTER = 1'000;
	constexpr size_t INNER = 32;

	std::vector<std::atomic<int>> visits(OUTER * INNER);
	std::atomic<size_t> dispatches{0};
	std::atomic<size_t> stayedOnDispatcher{0};

	// The outer batch owns the pool, so every inner dispatch loses the claim.
	// The inner grain of 1 would otherwise force a pooled dispatch, which is
	// the shape that used to deadlock.
	Jobs::For(OUTER, 8, [&](size_t begin, size_t end) {
		for (size_t outer = begin; outer < end; outer++) {
			const std::thread::id dispatcher = std::this_thread::get_id();
			bool elsewhere = false;

			Jobs::For(INNER, 1, [&](size_t innerBegin, size_t innerEnd) {
				elsewhere = elsewhere || std::this_thread::get_id() != dispatcher;
				for (size_t inner = innerBegin; inner < innerEnd; inner++) {
					visits[outer * INNER + inner].fetch_add(1, std::memory_order_relaxed);
				}
			});

			dispatches.fetch_add(1, std::memory_order_relaxed);
			if (!elsewhere) {
				stayedOnDispatcher.fetch_add(1, std::memory_order_relaxed);
			}
		}
	});

	REQUIRE(dispatches.load() == OUTER);

	// Inline is not an optimisation the nested call may decline. A nested
	// dispatch that reached another thread would mean two batches were live at
	// once, which is the corruption this exists to prevent.
	REQUIRE(stayedOnDispatcher.load() == OUTER);
	REQUIRE(NotVisitedExactlyOnce(visits) == 0);
}

TEST_CASE("two threads dispatching at once both complete", "[jobs]") {
	Pool pool{4};

	constexpr size_t COUNT = 20'000;
	std::vector<std::atomic<int>> first(COUNT);
	std::vector<std::atomic<int>> second(COUNT);

	auto dispatch = [](std::vector<std::atomic<int>> &visits) {
		Jobs::For(visits.size(), 64, [&](size_t begin, size_t end) {
			for (size_t index = begin; index < end; index++) {
				visits[index].fetch_add(1, std::memory_order_relaxed);
			}
		});
	};

	// Which of the two wins the pool is not defined and does not matter. Both
	// return with every index visited exactly once, which is the contract.
	std::thread other([&] { dispatch(second); });
	dispatch(first);
	other.join();

	REQUIRE(NotVisitedExactlyOnce(first) == 0);
	REQUIRE(NotVisitedExactlyOnce(second) == 0);
}

TEST_CASE("inline and pooled dispatch produce the same result", "[jobs]") {
	constexpr size_t COUNT = 5'000;

	// The property the whole scheme rests on: losing the pool costs time and
	// changes nothing. Run the same body with no pool at all, then against
	// four workers, and compare.
	auto accumulate = [](std::vector<size_t> &output) {
		Jobs::For(output.size(), 64, [&](size_t begin, size_t end) {
			for (size_t index = begin; index < end; index++) {
				output[index] = index * 3 + 1;
			}
		});
	};

	std::vector<size_t> serial(COUNT);
	{
		Jobs::Stop();
		accumulate(serial);
	}

	std::vector<size_t> parallel(COUNT);
	{
		Pool pool{4};
		accumulate(parallel);
	}

	REQUIRE(serial == parallel);
}

TEST_CASE("an exception from a nested For reaches the outer caller", "[jobs]") {
	Pool pool{4};

	// The inner dispatch is inline, so it rethrows directly into the outer
	// body, which captures it as that batch's failure and hands it back here.
	REQUIRE_THROWS_AS(
		Jobs::For(
			1'000,
			8,
			[](size_t begin, size_t) {
				Jobs::For(4, 1, [&](size_t, size_t) {
					if (begin == 0) {
						throw std::runtime_error("nested range failed");
					}
				});
			}
		),
		std::runtime_error
	);
}

TEST_CASE("a throwing batch releases the pool", "[jobs]") {
	Pool pool{4};

	REQUIRE_THROWS_AS(
		Jobs::For(
			10'000,
			64,
			[](size_t begin, size_t) {
				if (begin == 0) {
					throw std::runtime_error("range failed");
				}
			}
		),
		std::runtime_error
	);

	// A leaked claim would not fail anything: every later dispatch would run
	// inline, which is correct and silently serial forever. So the assertion
	// has to be that the pool is participating again, not merely that the next
	// call returns.
	REQUIRE(PoolParticipates(100'000));
}

TEST_CASE("Start is idempotent and Stop is safe without Start", "[jobs]") {
	Jobs::Stop();
	REQUIRE(Jobs::WorkerCount() == 0);

	Jobs::Start(2);
	Jobs::Start(8);
	REQUIRE(Jobs::WorkerCount() == 2);

	Jobs::Stop();
	Jobs::Stop();
	REQUIRE(Jobs::WorkerCount() == 0);
}

// --- forcing the whole pipeline onto one thread ------------------------------

namespace {
	// Turns the flag on and puts it back, whatever the case does.
	//
	// A process-wide switch left on by a failing assertion would make every
	// later case in this binary run serially and pass for the wrong reason —
	// which is the one failure mode a test of a global switch has to close.
	struct ForcedSerial {
		ForcedSerial() {
			engine::parallel::SetForceSerialCompute(true);
		}
		~ForcedSerial() {
			engine::parallel::SetForceSerialCompute(false);
		}
	};
}

TEST_CASE("forcing serial compute runs every span on the caller", "[parallel][jobs]") {
	// **A real pool, deliberately.** Testing this with no workers would prove
	// nothing: the inline path is already taken when the pool is empty, so the
	// case has to be one that would otherwise dispatch.
	Pool pool(4);
	REQUIRE(Jobs::WorkerCount() == 4);

	constexpr size_t COUNT = 1u << 20u;

	std::set<std::thread::id> threads;
	std::mutex guard;

	{
		ForcedSerial forced;
		REQUIRE(engine::parallel::ForceSerialCompute());

		Jobs::For(COUNT, 64, [&](size_t begin, size_t end) {
			std::lock_guard lock(guard);
			threads.insert(std::this_thread::get_id());
			(void)begin;
			(void)end;
		});

		// The whole point: one thread, and it is this one. A span opened by any
		// other thread is a span `core::FrameGraph::Push` would refuse, which is
		// what the flag exists to prevent.
		CHECK(threads.size() == 1);
		CHECK(*threads.begin() == std::this_thread::get_id());
		CHECK(Jobs::LastBatch().Participants == 1);
	}

	// And it goes back. A switch that could not be turned off would be a
	// build option wearing a function's clothes.
	CHECK_FALSE(engine::parallel::ForceSerialCompute());

	threads.clear();
	Jobs::For(COUNT, 64, [&](size_t begin, size_t end) {
		std::lock_guard lock(guard);
		threads.insert(std::this_thread::get_id());
		(void)begin;
		(void)end;
	});
	CHECK(threads.size() > 1);
}

TEST_CASE("a forced dispatch still visits every index exactly once", "[parallel][jobs]") {
	// **The flag changes wall time and nothing else.** `Jobs::For` promises
	// inline and pooled execution are observationally identical, and a
	// measurement instrument that quietly changed a result would make every
	// reading taken through it worthless.
	Pool pool(4);
	ForcedSerial forced;

	constexpr size_t COUNT = 10'000;
	std::vector<int> visits(COUNT, 0);

	Jobs::For(COUNT, 128, [&](size_t begin, size_t end) {
		for (size_t at = begin; at < end; at++) {
			visits[at]++;
		}
	});

	CHECK(std::count(visits.begin(), visits.end(), 1) == static_cast<long>(COUNT));
}

TEST_CASE("a forced dispatch still rethrows on the caller", "[parallel][jobs]") {
	Pool pool(4);
	ForcedSerial forced;

	CHECK_THROWS_AS(
		Jobs::For(1u << 20u, 64, [](size_t, size_t) { throw std::runtime_error("boom"); }), std::runtime_error
	);

	// The pool is released, so the next dispatch is not stranded — the same
	// property the pooled path promises, checked on this path too. It would
	// hang rather than fail if it were wrong, which is why it is a bare call
	// and not an assertion.
	Jobs::For(16, 4, [](size_t, size_t) {});
}
