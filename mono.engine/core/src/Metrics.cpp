#include <engine/core/Clock.hpp>
#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>

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

		// Indexed lookup regressed on measured 1 to 8 row cards; keep those sizes linear.
		constexpr size_t LINEAR_SCAN_ROW_LIMIT = 8;
		using RowIndex = std::vector<std::pair<uint32_t, size_t>>;

		struct Sink {
			std::mutex Guard;
			std::vector<Counter> Counters;
			std::vector<Level> Gauges;
			std::vector<Distribution> Histograms;
			RowIndex CounterIndex;
			RowIndex GaugeIndex;
			RowIndex HistogramIndex;
		};

		// **Not named `Get`**, which it was until v0.19: `Metrics::Get` is now a
		// member, and a `Get()` inside one of these functions would resolve to
		// it and fail to compile - or worse, recurse.
		Sink &MetricSink() {
			static Sink sink;
			return sink;
		}

		// Index ids separately so Drain retains registration order and sparse reserved
		// ids do not determine storage size. All access stays under the sink lock.
		template <class T> T *Lookup(std::vector<T> &rows, const RowIndex &index, Name name) {
			if (rows.size() <= LINEAR_SCAN_ROW_LIMIT) {
				const auto found =
					std::find_if(rows.begin(), rows.end(), [name](const T &row) { return row.Name == name; });
				return found != rows.end() ? &*found : nullptr;
			}

			const auto found =
				std::lower_bound(index.begin(), index.end(), name.Id(), [](const auto &entry, uint32_t id) {
					return entry.first < id;
				});
			return found != index.end() && found->first == name.Id() ? &rows[found->second] : nullptr;
		}

		template <class T> T &AppendIndexed(std::vector<T> &rows, RowIndex &index, T row) {
			const auto position = std::lower_bound(
				index.begin(), index.end(), row.Name.Id(), [](const auto &entry, uint32_t id) {
					return entry.first < id;
				}
			);
			const auto inserted = index.insert(position, {row.Name.Id(), rows.size()});
			try {
				rows.push_back(std::move(row));
			} catch (...) {
				// Row allocation must not leave an index pointing past the rows.
				index.erase(inserted);
				throw;
			}
			return rows.back();
		}

		Counter &Find(std::vector<Counter> &rows, RowIndex &index, Name name, bool isTime) {
			if (auto *existing = Lookup(rows, index, name)) return *existing;
			return AppendIndexed(rows, index, Counter{name, 0.0, 0, isTime});
		}

		Level &FindLevel(std::vector<Level> &rows, RowIndex &index, Name name) {
			if (auto *existing = Lookup(rows, index, name)) return *existing;
			return AppendIndexed(rows, index, Level{name, 0.0, 0});
		}

		Distribution &
		FindDistribution(std::vector<Distribution> &rows, RowIndex &index, Name name, bool isTime) {
			if (auto *existing = Lookup(rows, index, name)) return *existing;
			Distribution created;
			created.Name = name;
			created.IsTime = isTime;
			created.Window.resize(Metrics::RETAINED_OBSERVATIONS);
			return AppendIndexed(rows, index, std::move(created));
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

			// The fixed retained window trades one report allocation for 8 KiB of
			// stack scratch. Only its fully copied prefix participates in sorting.
			std::array<double, Metrics::RETAINED_OBSERVATIONS> sorted;
			std::copy_n(shape.Window.begin(), retained, sorted.begin());
			std::sort(sorted.begin(), sorted.begin() + retained);

			const auto at = [&sorted, retained](double fraction) {
				const auto rank = static_cast<size_t>(fraction * static_cast<double>(retained - 1) + 0.5);
				return sorted[std::min(rank, static_cast<size_t>(retained - 1))];
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

		auto &counter = Find(sink.Counters, sink.CounterIndex, Name(name), false);
		counter.Value += amount;
		counter.Samples++;
	}

	void Metrics::CountTime(std::string_view name, uint64_t nanoseconds) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);

		auto &counter = Find(sink.Counters, sink.CounterIndex, Name(name), true);
		counter.Value += static_cast<double>(nanoseconds);
		counter.Samples++;
	}

	void Metrics::SetGauge(std::string_view name, double value) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);

		auto &gauge = FindLevel(sink.Gauges, sink.GaugeIndex, Name(name));
		gauge.Value = value;
		gauge.Writes++;
	}

	void Metrics::Observe(std::string_view name, double value) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);
		RecordSample(FindDistribution(sink.Histograms, sink.HistogramIndex, Name(name), false), value);
	}

	void Metrics::ObserveTime(std::string_view name, uint64_t nanoseconds) {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);
		RecordSample(
			FindDistribution(sink.Histograms, sink.HistogramIndex, Name(name), true),
			static_cast<double>(nanoseconds)
		);
	}

	std::optional<Counter> Metrics::Get(std::string_view name) {
		auto &sink = MetricSink();
		const Name wanted(name);

		std::lock_guard lock(sink.Guard);
		if (const auto *counter = Lookup(sink.Counters, sink.CounterIndex, wanted)) return *counter;
		return std::nullopt;
	}

	std::optional<Gauge> Metrics::GetGauge(std::string_view name) {
		auto &sink = MetricSink();
		const Name wanted(name);

		std::lock_guard lock(sink.Guard);
		if (const auto *gauge = Lookup(sink.Gauges, sink.GaugeIndex, wanted))
			return Gauge{gauge->Name, gauge->Value, gauge->Writes};
		return std::nullopt;
	}

	std::optional<Histogram> Metrics::GetHistogram(std::string_view name) {
		auto &sink = MetricSink();
		const Name wanted(name);

		std::lock_guard lock(sink.Guard);
		if (const auto *shape = Lookup(sink.Histograms, sink.HistogramIndex, wanted)) return Rendered(*shape);
		return std::nullopt;
	}

	std::vector<Counter> Metrics::Drain() {
		auto &sink = MetricSink();
		std::lock_guard lock(sink.Guard);

		std::vector<Counter> drained;
		drained.swap(sink.Counters);
		sink.CounterIndex.clear();
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
		sink.CounterIndex.clear();
		sink.GaugeIndex.clear();
		sink.HistogramIndex.clear();
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
