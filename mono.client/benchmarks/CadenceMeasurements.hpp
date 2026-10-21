#pragma once

// Device-free C02 workload measurement. Verification and reference worlds stay
// outside the measured owner. The normal BENCH row includes the whole harness.
#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/UpdateCadence.hpp>
#include <engine/script/Runtime.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace cadence_bench {

	using engine::core::CFrame;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::scene::UpdateDomain;
	inline constexpr float DELTA = 1.0f / 60.0f;
	inline constexpr size_t WARM_TICKS = 120;
	inline constexpr size_t HISTORY_TICKS = 16;
	// Fresh batches keep finite-scene and clock oracles within the default baseline lifetime.
	inline constexpr size_t PROMOTION_BATCHES = 16;
	inline constexpr size_t PROMOTION_ROUNDS = 8;
	inline constexpr size_t PROMOTION_SETTLE_TICKS = 16;
	inline constexpr size_t PROMOTION_TRIGGER_COUNT = 4;
	inline constexpr size_t PROMOTION_BATCH_TICKS =
		PROMOTION_ROUNDS * PROMOTION_TRIGGER_COUNT * (PROMOTION_SETTLE_TICKS + 1);
	inline constexpr size_t PROMOTION_TICKS = PROMOTION_BATCHES * PROMOTION_BATCH_TICKS;
	inline constexpr std::array<std::string_view, 6> PHASES{
		"cadence npc",
		"cadence physics",
		"cadence animation clock",
		"cadence pose",
		"cadence beam",
		"cadence heartbeat"
	};
	inline void Require(bool condition, const char *message) {
		if (!condition) throw std::runtime_error(message);
	}
	// One bounded text snapshot per declared workload. Repeated harness warmups
	// replace it; process exit writes only each workload's latest completed cohort.
	struct LatestSummary {
		static constexpr size_t MAXIMUM_BYTES = 65536;
		std::array<char, 64> Row{};
		std::array<char, MAXIMUM_BYTES> Text{};
		size_t Size = 0;
		template <class... Values> void Append(const char *format, Values... values) {
			if constexpr (sizeof...(Values) == 0) {
				const std::string_view text(format);
				Require(text.size() < Text.size() - Size, "cadence summary exceeded bounded report storage");
				std::copy(text.begin(), text.end(), Text.begin() + static_cast<std::ptrdiff_t>(Size));
				Size += text.size();
				Text[Size] = '\0';
			} else {
				const int written = std::snprintf(Text.data() + Size, Text.size() - Size, format, values...);
				Require(
					written >= 0 && static_cast<size_t>(written) < Text.size() - Size,
					"cadence summary exceeded bounded report storage"
				);
				Size += static_cast<size_t>(written);
			}
		}
	};
	struct SummaryRegistry {
		std::array<LatestSummary, 20> Rows;
		~SummaryRegistry() {
			for (const auto &row : Rows)
				if (row.Size != 0) (void)std::fwrite(row.Text.data(), 1, row.Size, stdout);
		}
	};
	inline LatestSummary &BeginSummary(std::string_view name, size_t call) {
		static SummaryRegistry summaries;
		Require(name.size() < summaries.Rows.front().Row.size(), "cadence summary row name too long");
		for (auto &summary : summaries.Rows) {
			const std::string_view existing(summary.Row.data());
			if (!existing.empty() && existing != name) continue;
			std::copy(name.begin(), name.end(), summary.Row.begin());
			summary.Row[name.size()] = '\0';
			summary.Size = 0;
			summary.Append(
				"# cadence-cohort row=%s selection=latest_completed_body call=%zu "
				"retained_cohorts=1 emitted_at=process_exit\n",
				summary.Row.data(),
				call
			);
			return summary;
		}
		throw std::runtime_error("cadence summary workload count exceeded twenty rows");
	}
	// Snapshot only at cohort boundaries. Same-basis live totals are additive;
	// the sum of individual peaks is deliberately not a simultaneous heap peak.
	struct VmMemoryWindow {
		static constexpr size_t MAXIMUM_VMS = 32;
		std::array<engine::script::Runtime *, MAXIMUM_VMS> Runtimes{};
		std::array<engine::script::VmMemorySnapshot, MAXIMUM_VMS> Before{}, After{};
		size_t Count = 0;
		void Begin(engine::script::Runtime &runtime) {
			Require(Count < MAXIMUM_VMS, "cadence VM memory snapshot capacity exceeded");
			runtime.BeginMemoryWindow();
			Runtimes[Count] = &runtime;
			Before[Count] = runtime.MemorySnapshot();
			Require(
				Before[Count].Basis != engine::script::VmMemoryBasis::Unavailable,
				"cadence main VM has no allocator memory snapshot"
			);
			if (Count != 0)
				Require(
					Before[Count].Basis == Before[0].Basis, "cadence mixed VM allocator bases in one total"
				);
			++Count;
		}
		void End() {
			for (size_t index = 0; index < Count; ++index) {
				After[index] = Runtimes[index]->MemorySnapshot();
				Require(After[index].Basis == Before[index].Basis, "cadence VM allocator basis changed");
			}
		}
		static void Add(uint64_t &total, uint64_t value) {
			Require(
				value <= std::numeric_limits<uint64_t>::max() - total, "cadence VM memory total overflow"
			);
			total += value;
		}
		static void OptionalField(LatestSummary &summary, const char *name, std::optional<uint64_t> value) {
			if (value)
				summary.Append(" %s=%llu", name, static_cast<unsigned long long>(*value));
			else
				summary.Append(" %s=unavailable", name);
		}
		void Summarize(LatestSummary &summary, const char *row) const {
			using engine::script::VmMemoryBasis;
			if (Count == 0) {
				summary.Append(
					"# cadence-vm-memory row=%s vm_count=0 basis=not_applicable window=measured_cohort\n", row
				);
				return;
			}
			const auto basis = Before[0].Basis;
			const bool requested = basis == VmMemoryBasis::RequestedBytes;
			Require(
				requested || basis == VmMemoryBasis::QuickJsChargedBytes,
				"cadence unrecognized VM allocator basis"
			);
			uint64_t bytesBefore = 0, bytesAfter = 0, blocksBefore = 0, blocksAfter = 0;
			std::optional<uint64_t> peaks = uint64_t{0}, growthBytes = uint64_t{0}, growthOps = uint64_t{0};
			for (size_t index = 0; index < Count; ++index) {
				const auto &before = Before[index];
				const auto &after = After[index];
				Add(bytesBefore, before.LiveBytes);
				Add(bytesAfter, after.LiveBytes);
				Add(blocksBefore, before.LiveBlocks);
				Add(blocksAfter, after.LiveBlocks);
				if (peaks && after.PeakLiveBytes) {
					Require(
						*after.PeakLiveBytes >= before.LiveBytes && *after.PeakLiveBytes >= after.LiveBytes,
						"cadence VM peak below live memory"
					);
					Add(*peaks, *after.PeakLiveBytes);
				} else
					peaks.reset();
				if (growthBytes && before.GrowthBytes && after.GrowthBytes &&
					*after.GrowthBytes >= *before.GrowthBytes)
					Add(*growthBytes, *after.GrowthBytes - *before.GrowthBytes);
				else
					growthBytes.reset();
				if (growthOps && before.GrowthOps && after.GrowthOps && *after.GrowthOps >= *before.GrowthOps)
					Add(*growthOps, *after.GrowthOps - *before.GrowthOps);
				else
					growthOps.reset();
			}
			summary.Append(
				"# cadence-vm-memory row=%s vm_count=%zu scope=main_vms reference_vms=excluded "
				"window=measured_cohort snapshot_outside_owner=1 basis=%s "
				"live_bytes_measure=%s live_blocks_measure=%s live_bytes_before=%llu live_bytes_after=%llu "
				"live_blocks_before=%llu live_blocks_after=%llu peak_aggregation=%s "
				"cross_basis_comparison=invalid rss_attribution=none",
				row,
				Count,
				requested ? "requested_bytes" : "quickjs_charged_bytes",
				requested ? "allocator_requested_size" : "malloc_size",
				requested ? "live_allocator_blocks" : "malloc_count",
				static_cast<unsigned long long>(bytesBefore),
				static_cast<unsigned long long>(bytesAfter),
				static_cast<unsigned long long>(blocksBefore),
				static_cast<unsigned long long>(blocksAfter),
				requested ? "sum_of_individual_vm_peaks" : "unavailable"
			);
			OptionalField(summary, "sum_vm_window_peaks_bytes", peaks);
			OptionalField(summary, "positive_requested_growth_bytes_delta", growthBytes);
			OptionalField(summary, "positive_requested_growth_ops_delta", growthOps);
			summary.Append(
				" growth_basis=%s\n",
				requested ? "successful_positive_requested_size_changes_only" : "unavailable"
			);
		}
	};
	inline size_t TickCount() {
		const char *text = std::getenv("ATOMIC_CADENCE_TICKS");
		if (text == nullptr) return 600;
		char *end = nullptr;
		const unsigned long value = std::strtoul(text, &end, 10);
		Require(
			end != text && *end == '\0' && value >= 120 && value <= 3600,
			"ATOMIC_CADENCE_TICKS must be in 120..3600"
		);
		return static_cast<size_t>(value);
	}
	inline bool Near(float left, float right, float tolerance = .0002f) {
		return std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= tolerance;
	}
	inline bool Near(const Vector3 &left, const Vector3 &right, float tolerance = .0002f) {
		return Near(left.X, right.X, tolerance) && Near(left.Y, right.Y, tolerance) &&
			   Near(left.Z, right.Z, tolerance);
	}
	inline bool Near(const CFrame &left, const CFrame &right) {
		return Near(left.Position, right.Position) && Near(left.QuaternionX, right.QuaternionX) &&
			   Near(left.QuaternionY, right.QuaternionY) && Near(left.QuaternionZ, right.QuaternionZ) &&
			   Near(left.QuaternionW, right.QuaternionW);
	}
	inline uint64_t AttackDeadline(const Store &store, Entity actor) {
		engine::ecs::AttributeValue value;
		Require(
			engine::ecs::GetAttribute(store, actor, Name("NextAttackTick"), value),
			"cadence missing attack deadline"
		);
		double number = -1;
		switch (value.Type) {
		case engine::ecs::PropertyType::Int32:
			number = value.Int32;
			break;
		case engine::ecs::PropertyType::Int64:
			number = static_cast<double>(value.Int64);
			break;
		case engine::ecs::PropertyType::Float:
			number = value.Float;
			break;
		case engine::ecs::PropertyType::Double:
			number = value.Double;
			break;
		default:
			break;
		}
		Require(
			std::isfinite(number) && number >= 0 && number <= 1000000 && std::floor(number) == number,
			"cadence attack deadline is not an integer tick"
		);
		return static_cast<uint64_t>(number);
	}
	inline void SetAttackDeadline(Store &store, Entity actor, uint64_t tick) {
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::Double;
		value.Double = static_cast<double>(tick);
		Require(
			engine::ecs::SetAttribute(store, actor, Name("NextAttackTick"), value),
			"cadence deadline write refused"
		);
	}

	enum class Window : size_t {
		Steady,
		Invalidation,
		SingleCombat,
		CombatBurst,
		PlayerOwned,
		Demotion,
		Count
	};
	inline constexpr std::array<const char *, static_cast<size_t>(Window::Count)> WINDOWS{
		"steady", "invalidation", "single-combat", "combat-burst", "player-owned", "demotion"
	};
	inline bool PromotionEnabled() {
		const char *setting = std::getenv("ATOMIC_CADENCE_PROMOTIONS");
		if (setting == nullptr) return false;
		const std::string_view value(setting);
		if (value == "1") return true;
		Require(value == "0", "ATOMIC_CADENCE_PROMOTIONS must be exactly 0 or 1");
		return false;
	}
	inline Window PromotionWindow(size_t tick) {
		Require(tick < PROMOTION_BATCH_TICKS, "cadence promotion tick outside one batch");
		constexpr std::array<Window, PROMOTION_TRIGGER_COUNT> TRIGGERS{
			Window::SingleCombat, Window::CombatBurst, Window::PlayerOwned, Window::Invalidation
		};
		const size_t triggerSpan = PROMOTION_SETTLE_TICKS + 1;
		const size_t group = (tick / triggerSpan) % PROMOTION_TRIGGER_COUNT;
		const size_t offset = tick % triggerSpan;
		if (offset == PROMOTION_SETTLE_TICKS) return TRIGGERS[group];
		if (offset < 3 && group != 0) return Window::Demotion;
		return Window::Steady;
	}
	inline bool PromotionEntry(size_t tick) {
		return tick % (PROMOTION_SETTLE_TICKS + 1) == PROMOTION_SETTLE_TICKS;
	}
	inline Window WindowAt(size_t tick, size_t count) {
		if (tick == count / 3) return Window::Invalidation;
		if (tick >= count * 4 / 10 && tick < count * 9 / 20) return Window::SingleCombat;
		if (tick >= count / 2 && tick < count * 11 / 20) return Window::CombatBurst;
		if (tick >= count * 6 / 10 && tick < count * 13 / 20) return Window::PlayerOwned;
		for (const size_t end : {count * 9 / 20, count * 11 / 20, count * 13 / 20})
			if (tick >= end && tick < end + 4) return Window::Demotion;
		return Window::Steady;
	}
	inline void VerifyPromotionSettled(Store &store) {
		const auto verify = [&store](const engine::scene::DomainCadenceState &state) {
			if (state.Generation == 0) return;
			Require(
				state.CriticalReasons == 0 && store.Time().Tick > state.HoldUntilTick,
				"cadence promotion settle retained critical or demotion state"
			);
		};
		store.Each<const engine::scene::UpdateCadenceState>(
			[&verify](Entity, const engine::scene::UpdateCadenceState &state) {
				for (const auto &domain : state.Domains)
					verify(domain);
			}
		);
		const auto *poses = store.Resource<engine::scene::PoseCadenceHistory>();
		const auto *beams = store.Resource<engine::effects::BeamCadenceCache>();
		Require(poses != nullptr, "cadence promotion settle missing pose history");
		Require(beams != nullptr, "cadence promotion settle missing beam cache");
		for (const auto &[_, state] : poses->Poses)
			verify(state);
		for (const auto &[_, entry] : beams->Beams)
			verify(entry.State);
	}
	inline void VerifyPromotionDemotion(Store &store) {
		const uint64_t tick = store.Time().Tick;
		size_t held = 0;
		const auto verify = [&tick, &held](const engine::scene::DomainCadenceState &state) {
			if (state.Generation == 0 || state.HoldUntilTick < tick) return;
			++held;
			Require(
				state.LastCommittedTick == tick, "cadence promotion demotion did not consume a fresh value"
			);
		};
		store.Each<const engine::scene::UpdateCadenceState>(
			[&verify](Entity, const engine::scene::UpdateCadenceState &state) {
				for (const auto &domain : state.Domains)
					verify(domain);
			}
		);
		const auto *poses = store.Resource<engine::scene::PoseCadenceHistory>();
		const auto *beams = store.Resource<engine::effects::BeamCadenceCache>();
		Require(poses != nullptr, "cadence promotion demotion missing pose history");
		Require(beams != nullptr, "cadence promotion demotion missing beam cache");
		for (const auto &[_, state] : poses->Poses)
			verify(state);
		for (const auto &[_, entry] : beams->Beams)
			verify(entry.State);
		Require(held > 0, "cadence promotion demotion had no held states");
	}
	inline bool Critical(Window window, size_t actor) {
		return window == Window::CombatBurst ||
			   (actor == 0 && (window == Window::SingleCombat || window == Window::PlayerOwned));
	}
	inline engine::scene::UpdateCadence Policy(std::string_view identity, bool full, Entity owner = {}) {
		engine::scene::UpdateCadence policy;
		policy.Identity = Name(identity);
		policy.Owner = owner;
		policy.DemotionDelayTicks = 3;
		if (!full) {
			policy.Domains[static_cast<size_t>(UpdateDomain::Perception)] = {8, 12};
			policy.Domains[static_cast<size_t>(UpdateDomain::Behaviour)] = {4, 8};
			policy.Domains[static_cast<size_t>(UpdateDomain::VisualPose)] = {4, 8};
			policy.Domains[static_cast<size_t>(UpdateDomain::CosmeticEffects)] = {4, 8};
		}
		return policy;
	}
	struct Work {
		uint64_t Perception = 0, Behaviour = 0, Rays = 0, RayHits = 0;
		uint64_t PoseEvaluated = 0, PoseSkipped = 0, PoseWrites = 0, BoneResolves = 0;
		uint64_t BeamRebuilt = 0, BeamSkipped = 0, BeamVertices = 0;
		uint64_t Deadlines = 0, Damage = 0, Promotions = 0;
		uint64_t MaximumAge = 0, PromotionLatency = 0;
		void Add(const Work &other) {
			Perception += other.Perception;
			Behaviour += other.Behaviour;
			Rays += other.Rays;
			RayHits += other.RayHits;
			PoseEvaluated += other.PoseEvaluated;
			PoseSkipped += other.PoseSkipped;
			PoseWrites += other.PoseWrites;
			BoneResolves += other.BoneResolves;
			BeamRebuilt += other.BeamRebuilt;
			BeamSkipped += other.BeamSkipped;
			BeamVertices += other.BeamVertices;
			Deadlines += other.Deadlines;
			Damage += other.Damage;
			Promotions += other.Promotions;
			MaximumAge = std::max(MaximumAge, other.MaximumAge);
			PromotionLatency = std::max(PromotionLatency, other.PromotionLatency);
		}
	};
	struct TickReading {
		Window Kind = Window::Steady;
		double OwnerMilliseconds = 0, InclusiveMilliseconds = 0, SelfMilliseconds = 0;
		double UnmarkedMilliseconds = 0;
		std::array<double, PHASES.size()> Inclusive{}, Self{};
		uint64_t AllocatedBytes = 0, AllocatedBlocks = 0;
		uint64_t OffThreadScopes = 0;
		double ReportedWorkMilliseconds = 0;
		int64_t LiveBytes = 0, LiveBlocks = 0, PeakBytes = 0, OverheadBytes = 0;
		Work Counts;
	};
	inline std::optional<uint64_t> ResidentBytes() {
#if defined(__linux__)
		FILE *input = std::fopen("/proc/self/statm", "r");
		if (input == nullptr) return {};
		unsigned long long virtualPages = 0, residentPages = 0;
		const int read = std::fscanf(input, "%llu %llu", &virtualPages, &residentPages);
		std::fclose(input);
		const long pageBytes = ::sysconf(_SC_PAGESIZE);
		if (read == 2 && pageBytes > 0) return residentPages * static_cast<uint64_t>(pageBytes);
#endif
		return {};
	}
	struct Measurements {
		bool PreviousEnabled = engine::core::FrameGraph::IsEnabled();
		const bool HeapAvailable = engine::core::HeapProfile::IsCompiledIn();
		std::vector<TickReading> Ticks;
		std::vector<double> Scratch;
		std::optional<uint64_t> ResidentBefore = ResidentBytes();
		explicit Measurements(size_t count) : Ticks(count), Scratch(count) {
			engine::core::FrameGraph::SetEnabled(true);
		}
		~Measurements() {
			engine::core::FrameGraph::SetEnabled(PreviousEnabled);
		}
		template <class Step> void Measure(size_t tick, Window window, Step &&step) {
			using engine::core::FrameGraph;
			using engine::core::HeapProfile;
			auto &reading = Ticks[tick];
			reading.Kind = window;
			FrameGraph::BeginFrame();
			engine::core::HeapTotals before;
			if (HeapAvailable) before = HeapProfile::Totals();
			const auto started = std::chrono::steady_clock::now();
			{
				ENGINE_PROFILE_CAT("cadence tick", engine::core::ProfileCategory::Simulation);
				step(reading.Counts);
			}
			const auto ended = std::chrono::steady_clock::now();
			engine::core::HeapTotals after;
			if (HeapAvailable) after = HeapProfile::Totals();
			FrameGraph::EndFrame();
			Require(FrameGraph::OwnerDropped() == 0, "cadence owner frame dropped spans");
			reading.OffThreadScopes = FrameGraph::OffThreadDropped();
			reading.OwnerMilliseconds = std::chrono::duration<double, std::milli>(ended - started).count();
			if (HeapAvailable) {
				Require(
					after.DroppedScopes == before.DroppedScopes, "cadence heap attribution dropped scopes"
				);
				reading.AllocatedBytes = after.TotalBytes - before.TotalBytes;
				reading.AllocatedBlocks = after.TotalBlocks - before.TotalBlocks;
				reading.LiveBytes = after.LiveBytes;
				reading.LiveBlocks = after.LiveBlocks;
				reading.PeakBytes = after.PeakBytes;
				reading.OverheadBytes = after.OverheadBytes;
			}
			reading.UnmarkedMilliseconds = FrameGraph::UnmarkedMilliseconds();
			const auto &spans = FrameGraph::Spans();
			Require(!spans.empty() && spans.front().Name == "cadence tick", "cadence missing owner span");
			reading.InclusiveMilliseconds = spans.front().Milliseconds;
			reading.SelfMilliseconds = spans.front().SelfMilliseconds;
			for (const auto &span : spans) {
				if (span.Reported) {
					reading.ReportedWorkMilliseconds += span.SelfMilliseconds;
					continue;
				}
				Require(
					std::isfinite(span.Milliseconds) && std::isfinite(span.SelfMilliseconds) &&
						span.Milliseconds >= 0 && span.SelfMilliseconds >= 0,
					"cadence invalid measured span"
				);
				const auto found = std::find(PHASES.begin(), PHASES.end(), span.Name);
				if (found == PHASES.end()) continue;
				const size_t phase = static_cast<size_t>(found - PHASES.begin());
				reading.Inclusive[phase] += span.Milliseconds;
				reading.Self[phase] += span.SelfMilliseconds;
			}
		}
		template <class Value> double Percentile(Window window, double fraction, Value value) {
			size_t count = 0;
			for (const auto &tick : Ticks)
				if (tick.Kind == window) Scratch[count++] = value(tick);
			Require(count != 0, "cadence missing measurement window");
			std::sort(Scratch.begin(), Scratch.begin() + static_cast<std::ptrdiff_t>(count));
			return Scratch[static_cast<size_t>(std::ceil(fraction * static_cast<double>(count))) - 1];
		}
		void Summarize(
			LatestSummary &summary,
			const char *row,
			size_t call,
			size_t actors,
			size_t worlds,
			size_t joints,
			size_t keys,
			size_t beams,
			size_t obstacles,
			bool full,
			uint64_t retainedPayload,
			bool includesReference = true,
			size_t warmTicks = WARM_TICKS
		) {
			const auto residentAfter = ResidentBytes();
			summary.Append(
				"# cadence-config row=%s call=%zu actors=%zu worlds=%zu "
				"joints_per_rig=%zu keys_per_channel=%zu beams=%zu obstacles=%zu "
				"full=%d fixed_hz=60 warm_ticks=%zu measured_ticks=%zu backend=headless "
				"gpu=not-used vm_allocator_memory=reported_separately heap_compiled=%d "
				"measurement_mode=%s heap_coverage=%s "
				"owner_time=refresh_consumption_excludes_authored_policy_edits "
				"process_memory_includes_reference_and_oracles=%d retained_payload_is_main_cache_only=1 "
				"retained_cache_payload_bytes=%llu os_rss_before_bytes=%lld os_rss_after_bytes=%lld "
				"normal_BENCH_row=%s\n",
				row,
				call,
				actors,
				worlds,
				joints,
				keys,
				beams,
				obstacles,
				full,
				warmTicks,
				Ticks.size(),
				HeapAvailable,
				HeapAvailable ? "timing_with_heap_diagnostics" : "timing",
				HeapAvailable ? "cxx_new_delete" : "unavailable",
				includesReference,
				static_cast<unsigned long long>(retainedPayload),
				ResidentBefore ? static_cast<long long>(*ResidentBefore) : -1,
				residentAfter ? static_cast<long long>(*residentAfter) : -1,
				includesReference ? "whole_harness_including_reference_and_verification"
								  : "whole_harness_including_zero_work_verification"
			);
			for (size_t kind = 0; kind < WINDOWS.size(); ++kind) {
				const auto window = static_cast<Window>(kind);
				Work counts;
				size_t count = 0;
				uint64_t bytes = 0, blocks = 0, offThread = 0;
				double inclusive = 0, self = 0, unmarked = 0, reported = 0;
				const TickReading *last = nullptr;
				for (const auto &tick : Ticks) {
					if (tick.Kind != window) continue;
					++count;
					last = &tick;
					counts.Add(tick.Counts);
					if (HeapAvailable) {
						bytes += tick.AllocatedBytes;
						blocks += tick.AllocatedBlocks;
					}
					inclusive += tick.InclusiveMilliseconds;
					self += tick.SelfMilliseconds;
					unmarked += tick.UnmarkedMilliseconds;
					offThread += tick.OffThreadScopes;
					reported += tick.ReportedWorkMilliseconds;
				}
				Require(last != nullptr, "cadence empty measurement window");
				const auto owner = [](const TickReading &tick) { return tick.OwnerMilliseconds; };
				summary.Append(
					"# cadence-window row=%s call=%zu window=%s ticks=%zu tail_status=%s owner_p50_ms=%.6f "
					"owner_p95_ms=%.6f owner_p99_ms=%.6f owner_max_ms=%.6f inclusive_total_ms=%.6f "
					"self_total_ms=%.6f unmarked_total_ms=%.6f owner_frame_drops=0 off_thread_scopes=%llu "
					"reported_work_total_ms=%.6f ",
					row,
					call,
					WINDOWS[kind],
					count,
					count < 100 ? "insufficient_samples_below_100" : "sample_count_at_least_100",
					Percentile(window, .5, owner),
					Percentile(window, .95, owner),
					Percentile(window, .99, owner),
					Percentile(window, 1, owner),
					inclusive,
					self,
					unmarked,
					static_cast<unsigned long long>(offThread),
					reported
				);
				if (HeapAvailable)
					summary.Append(
						"heap_drops=0 allocated_bytes=%llu allocated_blocks=%llu process_live_bytes=%lld "
						"process_live_blocks=%lld process_peak_bytes=%lld profiler_overhead_bytes=%lld ",
						static_cast<unsigned long long>(bytes),
						static_cast<unsigned long long>(blocks),
						static_cast<long long>(last->LiveBytes),
						static_cast<long long>(last->LiveBlocks),
						static_cast<long long>(last->PeakBytes),
						static_cast<long long>(last->OverheadBytes)
					);
				else
					summary.Append(
						"heap_drops=unavailable allocated_bytes=unavailable allocated_blocks=unavailable "
						"process_live_bytes=unavailable process_live_blocks=unavailable "
						"process_peak_bytes=unavailable profiler_overhead_bytes=unavailable "
					);
				summary.Append(
					"perception=%llu behaviour=%llu ray_attempts=%llu ray_hits=%llu pose_evaluated=%llu "
					"pose_skipped=%llu pose_bone_writes=%llu bone_resolves=%llu beam_rebuilt=%llu "
					"beam_skipped=%llu beam_vertices=%llu deadline_checks=%llu damage=%llu "
					"pose_promotions=%llu maximum_age_ticks=%llu promotion_latency_ticks=%llu\n",
					static_cast<unsigned long long>(counts.Perception),
					static_cast<unsigned long long>(counts.Behaviour),
					static_cast<unsigned long long>(counts.Rays),
					static_cast<unsigned long long>(counts.RayHits),
					static_cast<unsigned long long>(counts.PoseEvaluated),
					static_cast<unsigned long long>(counts.PoseSkipped),
					static_cast<unsigned long long>(counts.PoseWrites),
					static_cast<unsigned long long>(counts.BoneResolves),
					static_cast<unsigned long long>(counts.BeamRebuilt),
					static_cast<unsigned long long>(counts.BeamSkipped),
					static_cast<unsigned long long>(counts.BeamVertices),
					static_cast<unsigned long long>(counts.Deadlines),
					static_cast<unsigned long long>(counts.Damage),
					static_cast<unsigned long long>(counts.Promotions),
					static_cast<unsigned long long>(counts.MaximumAge),
					static_cast<unsigned long long>(counts.PromotionLatency)
				);
				for (size_t phase = 0; phase < PHASES.size(); ++phase) {
					if (std::none_of(Ticks.begin(), Ticks.end(), [phase, window](const TickReading &tick) {
							return tick.Kind == window && tick.Inclusive[phase] > 0;
						}))
						continue;
					const auto phaseInclusive = [phase](const TickReading &tick) {
						return tick.Inclusive[phase];
					};
					const auto phaseSelf = [phase](const TickReading &tick) { return tick.Self[phase]; };
					summary.Append(
						"# cadence-phase row=%s call=%zu window=%s phase=%zu name=\"%.*s\" "
						"inclusive_p50_ms=%.6f inclusive_p99_ms=%.6f inclusive_max_ms=%.6f "
						"self_p50_ms=%.6f self_p99_ms=%.6f self_max_ms=%.6f\n",
						row,
						call,
						WINDOWS[kind],
						phase,
						static_cast<int>(PHASES[phase].size()),
						PHASES[phase].data(),
						Percentile(window, .5, phaseInclusive),
						Percentile(window, .99, phaseInclusive),
						Percentile(window, 1, phaseInclusive),
						Percentile(window, .5, phaseSelf),
						Percentile(window, .99, phaseSelf),
						Percentile(window, 1, phaseSelf)
					);
				}
			}
		}
	};

	struct BeamSet {
		std::vector<Entity> Beams, Ends;
		engine::effects::RibbonBuffer Output;
		void Create(
			Store &store, size_t count, bool full, size_t curveKeys, const std::vector<Entity> &owners = {}
		) {
			engine::effects::RegisterEffectClasses();
			for (size_t index = 0; index < count; ++index) {
				const auto first =
					store.CreateInstance(engine::ecs::Classes::Find(Name("Attachment")), "BeamStart");
				const auto last =
					store.CreateInstance(engine::ecs::Classes::Find(Name("Attachment")), "BeamEnd");
				engine::scene::Attachment start, finish;
				start.Frame.Position = {
					static_cast<float>(index % 32) * 5, 4, static_cast<float>(index / 32) * 5
				};
				finish.Frame.Position = start.Frame.Position + Vector3{3, 1, -2};
				store.Set(first, start);
				store.Set(last, finish);
				const auto beam =
					store.CreateInstance(engine::ecs::Classes::Find(Name("Beam")), "CosmeticBeam");
				engine::effects::Beam shape;
				shape.Attachment0 = first;
				shape.Attachment1 = last;
				shape.CurveSize0 = 2;
				shape.CurveSize1 = -1;
				shape.Width0 = .2f;
				shape.Width1 = 1.5f;
				shape.TextureSpeed = .25f;
				shape.Colour = {};
				shape.Transparency = {};
				for (size_t key = 0; key < curveKeys; ++key) {
					const float at = static_cast<float>(key) / static_cast<float>(curveKeys - 1);
					Require(
						shape.Colour.Add({at, {at, .25f + .5f * at, 1 - at}}), "cadence beam color overflow"
					);
					Require(shape.Transparency.Add({at, .15f + .3f * at}), "cadence beam alpha overflow");
				}
				store.Set(beam, shape);
				store.Set(
					beam,
					Policy(
						"bench.cosmetic." + std::to_string(index),
						full,
						owners.empty() ? Entity{} : owners[index % owners.size()]
					)
				);
				Beams.push_back(beam);
				Ends.push_back(last);
			}
		}
		void Author(Store &store, Window window) {
			const uint32_t combat = static_cast<uint32_t>(engine::scene::UpdateCriticality::Combat);
			for (size_t index = 0; index < Beams.size(); ++index) {
				auto *policy = store.GetMutable<engine::scene::UpdateCadence>(Beams[index]);
				policy->CriticalReasons =
					window == Window::CombatBurst || (window == Window::SingleCombat && index == 0) ? combat
																									: 0;
				if (window == Window::Invalidation) {
					++policy->Revision;
					store.GetMutable<engine::scene::Attachment>(Ends[index])->Frame.Position.Y += .5f;
				}
			}
		}
		void Build(Store &store, Work &counts) {
			ENGINE_PROFILE_CAT("cadence beam", engine::core::ProfileCategory::Simulation);
			const float elapsed = static_cast<float>(store.Time().Tick) * DELTA;
			const Vector3 eye{10 * std::sin(elapsed), 12, 20};
			Require(
				engine::effects::BuildRibbons(store, eye, elapsed, Output) == Beams.size(),
				"cadence missing beam geometry"
			);
			const auto *cache = store.Resource<engine::effects::BeamCadenceCache>();
			Require(
				cache != nullptr && cache->InvalidSources == 0 && cache->CapacityRefused == 0,
				"cadence beam source or capacity refusal"
			);
			counts.BeamRebuilt += cache->Rebuilt;
			counts.BeamSkipped += cache->Skipped;
			counts.BeamVertices += Output.Vertices.size();
		}
		uint64_t Payload(const Store &store) const {
			const auto *cache = store.Resource<engine::effects::BeamCadenceCache>();
			return cache == nullptr ? 0
									: cache->Beams.size() * sizeof(engine::effects::BeamCadenceCache::Entry);
		}
		void Verify(const Store &store, const BeamSet &reference, Window window, Work &counts) const {
			Require(
				Output.Vertices.size() == reference.Output.Vertices.size(), "cadence beam vertex mismatch"
			);
			Require(Output.Runs.size() == reference.Output.Runs.size(), "cadence beam run mismatch");
			for (size_t index = 0; index < Output.Vertices.size(); ++index) {
				const auto &actual = Output.Vertices[index];
				const auto &expected = reference.Output.Vertices[index];
				Require(
					Near(actual.Position, expected.Position) && actual.Coordinate == expected.Coordinate &&
						actual.Colour == expected.Colour,
					"cadence stale beam view, texture or geometry"
				);
			}
			const auto *cache = store.Resource<engine::effects::BeamCadenceCache>();
			for (size_t index = 0; index < Beams.size(); ++index) {
				const auto &state = cache->Beams.at(Beams[index]).State;
				const auto age = store.Time().Tick - state.LastCommittedTick;
				counts.MaximumAge = std::max(counts.MaximumAge, age);
				Require(age <= 8, "cadence beam exceeded staleness bound");
				if (Critical(window, index) || window == Window::Invalidation) {
					counts.PromotionLatency = std::max(counts.PromotionLatency, age);
					Require(age == 0, "cadence critical beam did not refresh this tick");
				}
			}
		}
	};
}
