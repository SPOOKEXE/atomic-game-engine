#pragma once

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
	inline unsigned ParseGpuBudget(const char *configured) {
		if (!configured) return 0;
		const std::string_view text(configured);
		unsigned percent = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), percent);
		if (error != std::errc{} || end != text.data() + text.size() || percent < 1 || percent > 100)
			throw std::runtime_error("MONO_GPU_BUDGET_PERCENT must be a decimal integer in 1..100");
		return percent;
	}

	inline bool GpuBudgetSuiteSupported(std::string_view suite) {
		return suite == "engine.render.bench.ray-consumers" ||
			   suite == "engine.render.bench.recording-batch" || suite == "engine.render.bench.weather-gpu";
	}

	// One dependency-closed burst starts idle and ends only after every owned GPU queue drains.
	// Each burst pays its own idle interval. Earlier idle and oversleep never buy future work.
	class BenchGpuPacing {
	  public:
		using Clock = std::chrono::steady_clock;
		using Time = std::chrono::time_point<Clock, std::chrono::nanoseconds>;
		const unsigned Percent;
		uint64_t CompletedBursts = 0, ActiveNanoseconds = 0, IdleNanoseconds = 0;
		uint64_t WaitNanoseconds = 0, WaitCalls = 0, MaximumBurstNanoseconds = 0;

		explicit BenchGpuPacing(unsigned percent = ParseGpuBudget(std::getenv("MONO_GPU_BUDGET_PERCENT")))
			: Percent(percent) {
			if (percent > 100) throw std::runtime_error("GPU budget percent exceeds 100");
		}
		void Begin() {
			if (Percent) Begin(Clock::now());
		}
		void Begin(Time now) {
			if (!Percent) return;
			if (Active || now < Ready) throw std::runtime_error("GPU burst began before budget settled");
			Started = now;
			Active = true;
		}
		void Complete() {
			if (Percent) Complete(Clock::now());
		}
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
		void Wait() {
			Wait(
				[] { return Time(Clock::now()); },
				[](Time deadline) { std::this_thread::sleep_until(deadline); }
			);
		}
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
