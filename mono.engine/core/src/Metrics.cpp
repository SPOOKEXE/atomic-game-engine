#include <engine/core/Clock.hpp>
#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <mutex>

namespace engine::core {

	namespace {

		struct Sink {
			std::mutex Guard;
			std::vector<Counter> Counters;
		};

		Sink &Get() {
			static Sink sink;
			return sink;
		}

		// Linear, because the counter set is tens of entries and stays that
		// way. A map here would cost an allocation per new name for no
		// measurable gain at this size.
		//
		// The comparison is on the interned id, so the scan is over integers
		// rather than strings — and adding a counter no longer allocates a
		// std::string per name per frame.
		Counter &Find(std::vector<Counter> &counters, Name name, bool isTime) {
			auto existing = std::find_if(counters.begin(), counters.end(), [name](const Counter &counter) {
				return counter.Name == name;
			});
			if (existing != counters.end()) {
				return *existing;
			}

			counters.push_back(Counter{name, 0.0, 0, isTime});
			return counters.back();
		}
	}

	void Metrics::Count(std::string_view name, double amount) {
		auto &sink = Get();
		std::lock_guard lock(sink.Guard);

		auto &counter = Find(sink.Counters, Name(name), false);
		counter.Value += amount;
		counter.Samples++;
	}

	void Metrics::CountTime(std::string_view name, uint64_t nanoseconds) {
		auto &sink = Get();
		std::lock_guard lock(sink.Guard);

		auto &counter = Find(sink.Counters, Name(name), true);
		counter.Value += static_cast<double>(nanoseconds);
		counter.Samples++;
	}

	std::vector<Counter> Metrics::Drain() {
		auto &sink = Get();
		std::lock_guard lock(sink.Guard);

		std::vector<Counter> drained;
		drained.swap(sink.Counters);
		return drained;
	}

	void Metrics::Clear() {
		auto &sink = Get();
		std::lock_guard lock(sink.Guard);
		sink.Counters.clear();
	}

	ScopedCount::ScopedCount(std::string_view name)
		: CounterName(name), StartNanoseconds(Clock::Nanoseconds()) {}

	ScopedCount::~ScopedCount() {
		Metrics::CountTime(CounterName, Clock::Nanoseconds() - StartNanoseconds);
	}
}
