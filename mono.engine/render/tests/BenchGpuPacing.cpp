#include "BenchGpuPacing.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <sstream>

TEST_SUITE_ID("engine.render.benchgpupacing")

namespace {
	using engine::testing::BenchGpuPacing;
	using Time = BenchGpuPacing::Time;
	Time At(int64_t nanoseconds) {
		return Time(std::chrono::nanoseconds(nanoseconds));
	}
}

TEST_CASE("GPU budget parsing and suite admission fail closed", "[render][gpu-budget]") {
	using namespace engine::testing;
	CHECK(ParseGpuBudget(nullptr) == 0);
	CHECK(ParseGpuBudget("25") == 25);
	CHECK(ParseGpuBudget("1") == 1);
	CHECK(ParseGpuBudget("100") == 100);
	for (const char *invalid : {"", "0", "101", "-1", "+25", " 25", "25 ", "25x", "999999999999999999999"}) {
		INFO(invalid);
		CHECK_THROWS(ParseGpuBudget(invalid));
	}
	CHECK(GpuBudgetSuiteSupported("engine.render.bench.ray-consumers"));
	CHECK(GpuBudgetSuiteSupported("engine.render.bench.weather-gpu"));
	CHECK(GpuBudgetSuiteSupported("engine.render.bench.recording-batch"));
	CHECK_FALSE(GpuBudgetSuiteSupported("engine.render.bench.c01"));
	CHECK_FALSE(GpuBudgetSuiteSupported("engine.render.bench.ray-consumers-extra"));
	CHECK_FALSE(GpuBudgetSuiteSupported(""));
}

TEST_CASE("GPU bursts pay three idle units per active unit at 25 percent", "[render][gpu-budget]") {
	BenchGpuPacing budget(25);
	budget.Begin(At(100));
	budget.Complete(At(110));
	CHECK(budget.CompletedBursts == 1);
	CHECK(budget.ActiveNanoseconds == 10);
	CHECK(budget.IdleNanoseconds == 30);
	CHECK_THROWS(budget.Begin(At(139)));
	Time now = At(110);
	unsigned sleeps = 0;
	budget.Wait(
		[&] { return now; },
		[&](Time deadline) {
			CHECK(deadline == At(140));
			now = ++sleeps == 1 ? At(130) : deadline;
		}
	);
	CHECK(sleeps == 2);
	CHECK(budget.WaitCalls == 2);
	CHECK(budget.WaitNanoseconds == 30);
	budget.Begin(now);
	budget.Complete(At(160));
	CHECK(budget.IdleNanoseconds == 90);
	CHECK(budget.MaximumBurstNanoseconds == 20);
}

TEST_CASE("GPU idle oversleep and long pauses cannot bank burst credit", "[render][gpu-budget]") {
	BenchGpuPacing budget(25);
	budget.Begin(At(0));
	budget.Complete(At(10));
	Time now = At(10);
	budget.Wait([&] { return now; }, [&](Time) { now = At(1000); });
	CHECK(budget.WaitNanoseconds == 990);
	budget.Begin(At(10000));
	budget.Complete(At(10010));
	CHECK_THROWS(budget.Begin(At(10039)));
	now = At(10010);
	budget.Wait(
		[&] { return now; },
		[&](Time deadline) {
			CHECK(deadline == At(10040));
			now = deadline;
		}
	);
	CHECK(budget.IdleNanoseconds == 60);
}

TEST_CASE("GPU quota rounds idle upward and permits zero duration and 100 percent", "[render][gpu-budget]") {
	BenchGpuPacing budget(30);
	budget.Begin(At(0));
	budget.Complete(At(1));
	CHECK(budget.IdleNanoseconds == 3);
	CHECK_THROWS(budget.Begin(At(3)));
	budget.Begin(At(4));
	budget.Complete(At(4));
	CHECK(budget.CompletedBursts == 2);
	CHECK(budget.IdleNanoseconds == 3);
	BenchGpuPacing full(100);
	full.Begin(At(0));
	full.Complete(At(10));
	full.Begin(At(10));
	full.Complete(At(20));
	CHECK(full.IdleNanoseconds == 0);
	for (unsigned percent = 1; percent <= 100; ++percent) {
		for (const uint64_t active :
			 {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{99}, uint64_t{1001}, uint64_t{1000001}}) {
			INFO("percent=" << percent << " active_ns=" << active);
			BenchGpuPacing quota(percent);
			quota.Begin(At(0));
			quota.Complete(At(static_cast<int64_t>(active)));
			const uint64_t idle = quota.IdleNanoseconds;
			CHECK((active + idle) * percent >= active * 100);
			if (idle > 0) CHECK((active + idle - 1) * percent < active * 100);
		}
	}
}

TEST_CASE("GPU quota rejects invalid ownership and overflowing durations", "[render][gpu-budget]") {
	CHECK_THROWS(BenchGpuPacing(101));
	BenchGpuPacing budget(25);
	CHECK_THROWS(budget.Complete(At(1)));
	budget.Begin(At(10));
	CHECK_THROWS(budget.Begin(At(20)));
	CHECK_THROWS(budget.Complete(At(9)));
	CHECK_THROWS(budget.Wait([] { return At(20); }, [](Time) {}));
	budget.Complete(At(20));
	CHECK_THROWS(budget.Complete(At(20)));
	const auto maximum = std::numeric_limits<int64_t>::max();
	BenchGpuPacing durationOverflow(1);
	durationOverflow.Begin(At(0));
	CHECK_THROWS(durationOverflow.Complete(At(maximum)));
	BenchGpuPacing deadlineOverflow(25);
	deadlineOverflow.Begin(At(maximum - 10));
	CHECK_THROWS(deadlineOverflow.Complete(At(maximum - 1)));
	BenchGpuPacing counterOverflow(100);
	counterOverflow.CompletedBursts = std::numeric_limits<uint64_t>::max();
	counterOverflow.Begin(At(0));
	CHECK_THROWS(counterOverflow.Complete(At(1)));
}

TEST_CASE("disabled GPU quota does no clock or sleep work", "[render][gpu-budget]") {
	BenchGpuPacing budget(0);
	budget.Begin(At(0));
	budget.Begin(At(0));
	budget.Complete(At(1));
	budget.Wait(
		[]() -> Time {
			FAIL("disabled quota queried clock");
			return At(0);
		},
		[](Time) { FAIL("disabled quota slept"); }
	);
	CHECK(budget.CompletedBursts == 0);
	std::ostringstream report;
	budget.Print(report);
	CHECK(report.str().find("gpu_budget_percent=0") != std::string::npos);
}
