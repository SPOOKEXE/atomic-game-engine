#pragma once

// Cooperative benchmark pacing over completed host envelopes, not GPU hardware timing.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace engine::testing {
	// Parses the optional 1..100 percent setting; absence disables pacing and malformed values throw.
	inline unsigned ParseGpuBudget(const char *configured) {
		if (!configured) return 0;
		const std::string_view text(configured);
		unsigned percent = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), percent);
		if (error != std::errc{} || end != text.data() + text.size() || percent < 1 || percent > 100)
			throw std::runtime_error("MONO_GPU_BUDGET_PERCENT must be a decimal integer in 1..100");
		return percent;
	}

	// Accepts exact suite IDs whose benchmark paths close every owned GPU burst.
	inline bool GpuBudgetSuiteSupported(std::string_view suite) {
		return suite == "engine.render.bench.ray-consumers" ||
			   suite == "engine.render.bench.recording-batch" || suite == "engine.render.bench.weather-gpu" ||
			   suite == "engine.render.bench.imagegraph_gpu" ||
			   suite == "engine.render.bench.grid-composite" || suite == "engine.render.bench.computegraph" ||
			   suite == "engine.render.bench.scriptcomputebridge";
	}

	// One dependency-closed burst starts idle and ends only after every owned GPU queue drains.
	// The host envelope includes CPU work between admission and confirmed drain, not GPU execution alone.
	// Each burst pays its own idle interval. Earlier idle and oversleep never buy future work.
	class BenchGpuPacing {
	  public:
		// Monotonic host clock used for admission and idle deadlines.
		using Clock = std::chrono::steady_clock;
		// Host timestamp with nanosecond resolution, also supplied by deterministic CPU tests.
		using Time = std::chrono::time_point<Clock, std::chrono::nanoseconds>;
		// Requested share of charged host envelopes; zero disables pacing.
		const unsigned Percent;
		// Bursts charged after the caller confirms all owned GPU queues are idle.
		uint64_t CompletedBursts = 0;
		// Sum of charged host envelopes, including their CPU work and device drain.
		uint64_t ActiveNanoseconds = 0;
		// Sum of required idle intervals after completion; CPU time before Wait can pay part of each
		// interval.
		uint64_t IdleNanoseconds = 0;
		// Host time spent inside Wait, including sleeper overshoot and its call overhead.
		uint64_t WaitNanoseconds = 0;
		// Sleeper calls, including retries when a sleeper returns before its deadline.
		uint64_t WaitCalls = 0;
		// Largest charged host envelope; an already submitted burst cannot be preempted.
		uint64_t MaximumBurstNanoseconds = 0;

		// Uses the environment setting by default; explicit zero disables pacing and values above 100 throw.
		explicit BenchGpuPacing(unsigned percent = ParseGpuBudget(std::getenv("MONO_GPU_BUDGET_PERCENT")))
			: Percent(percent) {
			if (percent > 100) throw std::runtime_error("GPU budget percent exceeds 100");
		}
		// Starts an idle, dependency-closed burst using the host clock; disabled pacing reads no clock.
		void Begin() {
			if (Percent) Begin(Clock::now());
		}
		// Admits a caller-confirmed idle burst at now; an open burst or unpaid deadline throws.
		void Begin(Time now) {
			if (!Percent) return;
			if (Active || now < Ready) throw std::runtime_error("GPU burst began before budget settled");
			Started = now;
			Active = true;
		}
		// Charges the burst using the host clock after the caller drains every owned queue; does not sleep.
		void Complete() {
			if (Percent) Complete(Clock::now());
		}
		// Charges through now and sets the idle deadline, rounding upward; invalid bounds or overflow throw.
		void Complete(Time now) {
			if (!Percent) return;
			if (!Active || now < Started) throw std::runtime_error("GPU burst completion has no valid start");
			const auto start = Started.time_since_epoch().count(), end = now.time_since_epoch().count();
			if (start < 0 || end < 0) throw std::runtime_error("GPU budget clock outside supported range");
			const uint64_t active = uint64_t(end) - uint64_t(start);
			const uint64_t factor = 100 - Percent;
			const uint64_t maximum = uint64_t(std::numeric_limits<int64_t>::max());
			const uint64_t whole = active / Percent;
			if (factor && whole > maximum / factor)
				throw std::runtime_error("GPU budget idle duration overflow");
			const uint64_t remainder = (active % Percent * factor + Percent - 1) / Percent;
			const uint64_t idle = whole * factor + remainder;
			if (idle > maximum || uint64_t(end) > maximum - idle)
				throw std::runtime_error("GPU budget deadline overflow");
			Add(ActiveNanoseconds, active);
			Add(IdleNanoseconds, idle);
			Add(CompletedBursts, 1);
			MaximumBurstNanoseconds = std::max(MaximumBurstNanoseconds, active);
			Ready = Time(std::chrono::nanoseconds(end + int64_t(idle)));
			Active = false;
		}
		// Pays the remaining idle deadline with supplied clock and sleeper, retrying early wakes without
		// credit.
		template <class Now, class Sleep> void Wait(Now now, Sleep sleep) {
			if (!Percent) return;
			if (Active) throw std::runtime_error("GPU budget wait before owned queues drained");
			const auto started = now();
			if (started.time_since_epoch().count() < 0)
				throw std::runtime_error("GPU budget clock outside supported range");
			while (now() < Ready) {
				Add(WaitCalls, 1);
				sleep(Ready);
			}
			const auto ended = now();
			if (ended < started) throw std::runtime_error("GPU budget clock went backwards");
			Add(WaitNanoseconds, uint64_t((ended - started).count()));
		}
		// Pays the remaining idle deadline with the steady host clock and sleep_until.
		void Wait() {
			Wait(
				[] { return Time(Clock::now()); },
				[](Time deadline) { std::this_thread::sleep_until(deadline); }
			);
		}
		// Appends policy and cumulative host counters separately from benchmark work timings.
		void Print(std::ostream &out) const {
			out << " gpu_budget_percent=" << Percent << " gpu_budget_completed_bursts=" << CompletedBursts
				<< " gpu_budget_active_envelope_ns=" << ActiveNanoseconds
				<< " gpu_budget_required_idle_ns=" << IdleNanoseconds
				<< " gpu_budget_wait_ns=" << WaitNanoseconds << " gpu_budget_wait_calls=" << WaitCalls
				<< " gpu_budget_max_burst_ns=" << MaximumBurstNanoseconds
				<< " gpu_budget_basis=host_envelope_bounding_owned_gpu_activity"
				<< " gpu_budget_scope=cooperative_average_not_sm_power_or_instantaneous_utilization";
		}

	  private:
		bool Active = false;
		Time Started{}, Ready{};
		static void Add(uint64_t &total, uint64_t value) {
			if (value > std::numeric_limits<uint64_t>::max() - total)
				throw std::runtime_error("GPU budget counter overflow");
			total += value;
		}
	};
}
