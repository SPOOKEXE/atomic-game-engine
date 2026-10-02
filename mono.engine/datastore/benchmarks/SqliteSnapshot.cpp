// Measures the durable provider against a complete shared-store snapshot.

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Name.hpp>
#include <engine/datastore/Sqlite.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/DataStore.hpp>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.datastore.bench.sqlite-snapshot")

namespace {
	using engine::core::Name;
	using engine::datastore::MakeSqliteDataStoreAdapter;
	using engine::world::BusKind;
	using engine::world::DataStoreStatus;
	using engine::world::SharedStoreEntry;

	constexpr size_t ENTRY_COUNT = 1024;
	constexpr size_t VALUE_BYTES = 256;

	constexpr std::array<std::string_view, 8> PHASES{
		"datastore sqlite load",
		"datastore sqlite save",
		"datastore sqlite open",
		"datastore sqlite prepare",
		"datastore sqlite schema",
		"datastore sqlite encode",
		"datastore sqlite decode",
		"datastore sqlite step"
	};

	using HeapReadings = std::array<engine::core::HeapNodeView, PHASES.size()>;

	HeapReadings ReadHeap() {
		using engine::core::HeapProfile;
		const uint32_t count = HeapProfile::NodeCount();
		if (count > 4096) throw std::runtime_error("SQLite profile heap tree exceeds diagnostic bound");
		HeapReadings readings{};
		for (uint32_t index = 1; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			for (size_t phase = 0; phase < PHASES.size(); ++phase) {
				if (node.Name != PHASES[phase]) continue;
				auto &reading = readings[phase];
				reading.TotalBytes += node.TotalBytes;
				reading.TotalBlocks += node.TotalBlocks;
				reading.LiveBytes += node.LiveBytes;
				reading.LiveBlocks += node.LiveBlocks;
				reading.PeakBytes += node.PeakBytes;
			}
		}
		return readings;
	}

	struct RestoreProfile {
		bool Enabled = engine::core::FrameGraph::IsEnabled();
		~RestoreProfile() {
			engine::core::FrameGraph::SetEnabled(Enabled);
		}
	};

	struct SnapshotFixture {
		std::filesystem::path Root =
			std::filesystem::current_path() / ".cache/build/bench/benchmark-data/datastore-sqlite";
		std::unique_ptr<engine::world::DataStoreAdapter> Adapter;
		std::vector<SharedStoreEntry> Entries;
		std::vector<SharedStoreEntry> Expected;
		std::vector<SharedStoreEntry> Loaded;
		std::string Error;
		std::array<size_t, 2> Calls{};

		SnapshotFixture() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
			Entries.reserve(ENTRY_COUNT);
			for (size_t index = 0; index < ENTRY_COUNT; ++index) {
				std::vector<std::byte> value(VALUE_BYTES, static_cast<std::byte>(index & 0xffu));
				Entries.push_back(
					{BusKind::DataStore, Name("entry." + std::to_string(index)), std::move(value), index + 1}
				);
			}
			// The portable image orders keys by spelling, independently of intern IDs.
			Expected = Entries;
			std::sort(Expected.begin(), Expected.end(), [](const auto &left, const auto &right) {
				return left.Key.Text() < right.Key.Text();
			});
			Adapter = MakeSqliteDataStoreAdapter(Root, engine::world::SharedStoreEnvironment::Live);
			if (Adapter->Save(Name("main"), Entries, Error) != DataStoreStatus::Ok) {
				throw std::runtime_error("could not seed SQLite benchmark snapshot: " + Error);
			}
			// Preflight the complete encoded snapshot before measuring identical saves.
			LoadRaw();
		}

		~SnapshotFixture() {
			Adapter.reset();
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}

		void SaveRaw() {
			if (Adapter->Save(Name("main"), Entries, Error) != DataStoreStatus::Ok) {
				throw std::runtime_error("SQLite benchmark save failed: " + Error);
			}
		}

		void LoadRaw() {
			if (Adapter->Load(Name("main"), Loaded, Error) != DataStoreStatus::Ok || Loaded != Expected) {
				throw std::runtime_error("SQLite benchmark load failed: " + Error);
			}
		}

		// Capture and counter reads are opt-in; their reporting belongs to the
		// broader BENCH cost. Compare normal timings only with normal timings.
		void Run(bool save) {
			using engine::core::FrameGraph;
			using engine::core::HeapProfile;
			static const bool profile = [] {
				const char *value = std::getenv("ATOMIC_SQLITE_PROFILE");
				return value != nullptr && std::string_view(value) == "1";
			}();
			if (!profile) {
				if (save)
					SaveRaw();
				else
					LoadRaw();
				return;
			}
			auto &calls = Calls[save ? 1 : 0];
			if (!HeapProfile::IsCompiledIn() || calls == 128)
				throw std::runtime_error("SQLite profile requires heap hooks and at most 128 calls per row");
			const RestoreProfile restore;
			const auto before = ReadHeap();
			const auto totalsBefore = HeapProfile::Totals();
			FrameGraph::SetEnabled(true);
			FrameGraph::BeginFrame();
			try {
				if (save)
					SaveRaw();
				else
					LoadRaw();
			} catch (...) {
				// Close the owner frame on refusal before restoring the collector setting.
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			const auto after = ReadHeap();
			const auto totalsAfter = HeapProfile::Totals();
			const auto &spans = FrameGraph::Spans();
			std::array<float, PHASES.size()> inclusive{}, self{};
			std::array<bool, PHASES.size()> found{};
			bool hierarchy = !spans.empty();
			for (size_t index = 0; index < spans.size(); ++index) {
				const auto &span = spans[index];
				if (span.Parent == FrameGraph::NO_PARENT)
					hierarchy &= span.Depth == 0;
				else
					hierarchy &= span.Parent < index && span.Depth == spans[span.Parent].Depth + 1;
				for (size_t phase = 0; phase < PHASES.size(); ++phase) {
					if (span.Name != PHASES[phase]) continue;
					found[phase] = true;
					inclusive[phase] += span.Milliseconds;
					self[phase] += span.SelfMilliseconds;
				}
			}
			const bool required = found[save ? 1 : 0] && found[2] && found[3] && found[7] &&
								  (save ? found[4] && found[5] : found[6]);
			if (!hierarchy || !required || FrameGraph::Dropped() != 0 ||
				totalsAfter.DroppedScopes != totalsBefore.DroppedScopes)
				throw std::runtime_error("SQLite profile incomplete hierarchy, phase or heap/frame drops");
			++calls;
			std::printf(
				"# sqlite-profile operation=%s call=%zu warmup=%d spans=%zu dropped=%zu "
				"heap_coverage=cxx_new_delete sqlite_c_heap=untracked heap_dropped_delta=%" PRIu64
				" frame_ms=%.6f unmarked_ms=%.6f overhead_bytes=%" PRId64,
				save ? "save" : "load",
				calls,
				calls <= 8,
				spans.size(),
				FrameGraph::Dropped(),
				totalsAfter.DroppedScopes - totalsBefore.DroppedScopes,
				FrameGraph::FrameMilliseconds(),
				FrameGraph::UnmarkedMilliseconds(),
				totalsAfter.OverheadBytes
			);
			for (size_t phase = 0; phase < PHASES.size(); ++phase) {
				if (!found[phase]) continue;
				std::printf(
					" phase%zu_inclusive_ms=%.6f phase%zu_self_ms=%.6f "
					"phase%zu_alloc_bytes=%" PRIu64 " phase%zu_alloc_blocks=%" PRIu64 " "
					"phase%zu_live_bytes_before=%" PRId64 " phase%zu_live_bytes_after=%" PRId64 " "
					"phase%zu_live_blocks_before=%" PRId64 " phase%zu_live_blocks_after=%" PRId64 " "
					"phase%zu_sum_tag_peak_bytes=%" PRId64,
					phase,
					inclusive[phase],
					phase,
					self[phase],
					phase,
					after[phase].TotalBytes - before[phase].TotalBytes,
					phase,
					after[phase].TotalBlocks - before[phase].TotalBlocks,
					phase,
					before[phase].LiveBytes,
					phase,
					after[phase].LiveBytes,
					phase,
					before[phase].LiveBlocks,
					phase,
					after[phase].LiveBlocks,
					phase,
					after[phase].PeakBytes
				);
			}
			std::printf("\n");
		}
	};

	SnapshotFixture &Fixture() {
		static SnapshotFixture fixture;
		return fixture;
	}
}

BENCH("SQLite atomic snapshot replace, 1024 entries x 256 bytes", 1) {
	Fixture().Run(true);
}

BENCH("SQLite snapshot load, 1024 entries x 256 bytes", 1) {
	Fixture().Run(false);
	engine::testing::Consume(Fixture().Loaded.size());
}
