#include <engine/core/Clock.hpp>
#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <mutex>

namespace engine::core {

	namespace {

		// A histogram's storage: the exact totals over everything, and a ring of
		// the most recent observations for the percentiles.
		//
		// **A ring rather than a growing vector**, because the alternative is a
		// sink whose footprint is a function of process uptime. The array is
		// allocated once with the entry and never resized, so observing costs
		// one store and some arithmetic.
		struct Distribution {
			core::Name Name;
			uint64_t Samples = 0;
			double Sum = 0.0;
			double Minimum = 0.0;
			double Maximum = 0.0;
			bool IsTime = false;

			std::vector<double> Window;
			uint32_t Next = 0;
			bool Wrapped = false;
		};

		struct Level {
			core::Name Name;
			double Value = 0.0;
			uint64_t Writes = 0;
		};

		struct Sink {
			std::mutex Guard;
			std::vector<Counter> Counters;
			std::vector<Level> Gauges;
			std::vector<Distribution> Histograms;
		};

		// **Not named `Get`**, which it was until v0.19: `Metrics::Get` is now a
		// member, and a `Get()` inside one of these functions would resolve to
		// it and fail to compile - or worse, recurse.
		Sink &MetricSink() {
			static Sink sink;
			return sink;
		}

		// Linear, because the counter set is tens of entries and stays that
		// way. A map here would cost an allocation per new name for no
		// measurable gain at this size.
		//
		// The comparison is on the interned id, so the scan is over integers
		// rather than strings - and adding a counter no longer allocates a
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

		Level &FindLevel(std::vector<Level> &gauges, Name name) {
			auto existing = std::find_if(gauges.begin(), gauges.end(), [name](const Level &gauge) {
				return gauge.Name == name;
			});
			if (existing != gauges.end()) {
				return *existing;
			}

			gauges.push_back(Level{name, 0.0, 0});
			return gauges.back();
		}

		Distribution &FindDistribution(std::vector<Distribution> &histograms, Name name, bool isTime) {
			auto existing =
				std::find_if(histograms.begin(), histograms.end(), [name](const Distribution &shape) {
					return shape.Name == name;
				});
			if (existing != histograms.end()) {
				return *existing;
			}

			Distribution created;
			created.Name = name;
			created.IsTime = isTime;

			// Once, with the entry. A histogram that grew its window would
			// allocate inside whatever hot path first observed into it.
			created.Window.resize(Metrics::RETAINED_OBSERVATIONS);
			histograms.push_back(std::move(created));
			return histograms.back();
		}

		void RecordSample(Distribution &shape, double value) {
			if (shape.Samples == 0) {
				shape.Minimum = value;
				shape.Maximum = value;
			} else {
				shape.Minimum = std::min(shape.Minimum, value);
				shape.Maximum = std::max(shape.Maximum, value);
			}

			shape.Samples++;
			shape.Sum += value;
			shape.Window[shape.Next] = value;
			shape.Next++;
			if (shape.Next == Metrics::RETAINED_OBSERVATIONS) {
				shape.Next = 0;
				shape.Wrapped = true;
			}
		}

		// The public form of one distribution, with its percentiles taken.
		//
		// Nearest-rank on a sorted copy, for `FrameGraph`'s reason: with
		// thousands of readings an interpolated percentile reports a value that
		// never happened.
		Histogram Rendered(const Distribution &shape) {
			Histogram out;
			out.Name = shape.Name;
			out.Samples = shape.Samples;
			out.Sum = shape.Sum;
			out.Minimum = shape.Minimum;
			out.Maximum = shape.Maximum;
			out.Mean = shape.Samples == 0 ? 0.0 : shape.Sum / static_cast<double>(shape.Samples);
			out.IsTime = shape.IsTime;

			const uint32_t retained = shape.Wrapped ? Metrics::RETAINED_OBSERVATIONS : shape.Next;
			out.Retained = retained;
			if (retained == 0) {
				return out;
			}

			std::vector<double> sorted(shape.Window.begin(), shape.Window.begin() + retained);
			std::sort(sorted.begin(), sorted.end());

			const auto at = [&sorted](double fraction) {
				const auto rank =
					static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1) + 0.5);
				return sorted[std::min(rank, sorted.size() - 1)];
			};
			out.P50 = at(0.50);
			out.P95 = at(0.95);
			out.P99 = at(0.99);
			return out;
		}

		// Sorted by the text rather than by the interned id, because a report is
		// read by a person and first-seen order is not an order anybody expects.
		template <class T> void ByName(std::vector<T> &rows) {
			std::sort(rows.begin(), rows.end(), [](const T &left, const T &right) {
				return left.Name.Text() < right.Name.Text();
			});
		}
	}

	void Metrics::Count(std::string_view name, double amount) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);

		auto &counter = Find(sink.Counters, Name(name), false);
		counter.Value += amount;
		counter.Samples++;
	}

	void Metrics::CountTime(std::string_view name, uint64_t nanoseconds) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);

		auto &counter = Find(sink.Counters, Name(name), true);
		counter.Value += static_cast<double>(nanoseconds);
		counter.Samples++;
	}

	void Metrics::SetGauge(std::string_view name, double value) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);

		auto &gauge = FindLevel(sink.Gauges, Name(name));
		gauge.Value = value;
		gauge.Writes++;
	}

	void Metrics::Observe(std::string_view name, double value) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);
		RecordSample(FindDistribution(sink.Histograms, Name(name), false), value);
	}

	void Metrics::ObserveTime(std::string_view name, uint64_t nanoseconds) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);
		RecordSample(FindDistribution(sink.Histograms, Name(name), true), static_cast<double>(nanoseconds));
	}

	std::optional<Counter> Metrics::Get(std::string_view name) {
		auto &sink = MetricSink();
		const Name wanted(name);

		std::lock_guard lock(sink.Guard);
		for (const Counter &counter : sink.Counters) {
			if (counter.Name == wanted) {
				return counter;
			}
		}
		return std::nullopt;
	}

	std::optional<Gauge> Metrics::GetGauge(std::string_view name) {
		auto &sink = MetricSink();
		const Name wanted(name);

		std::lock_guard lock(sink.Guard);
		for (const Level &gauge : sink.Gauges) {
			if (gauge.Name == wanted) {
				return Gauge{gauge.Name, gauge.Value, gauge.Writes};
			}
		}
		return std::nullopt;
	}

	std::optional<Histogram> Metrics::GetHistogram(std::string_view name) {
		auto &sink = MetricSink();
		const Name wanted(name);

		std::lock_guard lock(sink.Guard);
		for (const Distribution &shape : sink.Histograms) {
			if (shape.Name == wanted) {
				return Rendered(shape);
			}
		}
		return std::nullopt;
	}

	std::vector<Counter> Metrics::Drain() {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);

		std::vector<Counter> drained;
		drained.swap(sink.Counters);
		return drained;
	}

	MetricsSnapshot Metrics::Snapshot() {
		auto &sink = MetricSink();
		MetricsSnapshot taken;

		{
			std::lock_guard lock(sink.Guard);
			taken.Counters = sink.Counters;

			taken.Gauges.reserve(sink.Gauges.size());
			for (const Level &gauge : sink.Gauges) {
				taken.Gauges.push_back(Gauge{gauge.Name, gauge.Value, gauge.Writes});
			}

			taken.Histograms.reserve(sink.Histograms.size());
			for (const Distribution &shape : sink.Histograms) {
				taken.Histograms.push_back(Rendered(shape));
			}
		}

		ByName(taken.Counters);
		ByName(taken.Gauges);
		ByName(taken.Histograms);
		return taken;
	}

	void Metrics::Clear() {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);
		sink.Counters.clear();
		sink.Gauges.clear();
		sink.Histograms.clear();
	}

	ScopedCount::ScopedCount(std::string_view name)
		: CounterName(name), StartNanoseconds(Clock::Nanoseconds()) {}

	ScopedCount::~ScopedCount() {
		Metrics::CountTime(CounterName, Clock::Nanoseconds() - StartNanoseconds);
	}

	ScopedObservation::ScopedObservation(std::string_view name)
		: HistogramName(name), StartNanoseconds(Clock::Nanoseconds()) {}

	ScopedObservation::~ScopedObservation() {
		Metrics::ObserveTime(HistogramName, Clock::Nanoseconds() - StartNanoseconds);
	}
}
