#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <algorithm>
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
	Pool pool { 4 };

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
	Pool pool { 4 };

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
	Pool pool { 4 };

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
	Pool pool { 4 };

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
	std::atomic<size_t> total { 0 };

	Jobs::For(500, 16, [&](size_t begin, size_t end) {
		total.fetch_add(end - begin, std::memory_order_relaxed);
	});

	REQUIRE(total.load() == 500);
}

TEST_CASE("a count of zero does nothing", "[jobs]") {
	Pool pool { 2 };

	bool called = false;
	Jobs::For(0, 8, [&](size_t, size_t) {
		called = true;
	});

	REQUIRE_FALSE(called);
}

TEST_CASE("an exception in a range reaches the caller", "[jobs]") {
	Pool pool { 4 };

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
