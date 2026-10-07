#include <engine/core/BenchmarkReport.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>

namespace engine::core {
	BenchmarkReport::BenchmarkReport(const HeapTotals &initial)
		: Initial(initial), PeakBytes(initial.LiveBytes) {}

	void BenchmarkReport::AddFrame(
		std::span<const FrameSpan> spans,
		double milliseconds,
		size_t dropped,
		const HeapTotals &heap,
		size_t offThreadDropped
	) {
		Frames++;
		Dropped += dropped;
		OffThreadDropped += offThreadDropped;
		Valid = Valid && offThreadDropped <= dropped;
		PeakBytes = std::max(PeakBytes, heap.LiveBytes);
		Valid = Valid && std::isfinite(milliseconds) && milliseconds >= 0.0;
		Milliseconds[5] += milliseconds;
		for (size_t index = 0; index < spans.size(); index++) {
			const FrameSpan &span = spans[index];
			if (span.Reported) continue;
			const double inclusive = span.Milliseconds;
			Valid = Valid && std::isfinite(inclusive) && inclusive >= 0.0;
			bool sameCategoryAncestor = false;
			bool imagegraphAncestor = false;
			size_t parent = index;
			while (spans[parent].Depth > 0 && spans[parent].Parent < parent) {
				parent = spans[parent].Parent;
				const FrameSpan &ancestor = spans[parent];
				if (ancestor.Reported) continue;
				sameCategoryAncestor = sameCategoryAncestor || ancestor.Category == span.Category;
				imagegraphAncestor = imagegraphAncestor || ancestor.Name.starts_with("imagegraph");
			}
			if (!sameCategoryAncestor) {
				switch (span.Category) {
				case ProfileCategory::Render:
					WallMilliseconds[0] += inclusive;
					break;
				case ProfileCategory::Physics:
					WallMilliseconds[1] += inclusive;
					break;
				case ProfileCategory::Network:
					WallMilliseconds[2] += inclusive;
					break;
				default:
					break;
				}
			}
			if (span.Name.starts_with("imagegraph") && !imagegraphAncestor) WallMilliseconds[3] += inclusive;
			const double self = span.SelfMilliseconds;
			Valid = Valid && std::isfinite(self) && self >= 0.0;
			switch (span.Category) {
			case ProfileCategory::Render:
				Milliseconds[0] += self;
				break;
			case ProfileCategory::Physics:
				Milliseconds[1] += self;
				break;
			case ProfileCategory::Network:
				Milliseconds[2] += self;
				break;
			case ProfileCategory::Script:
				Milliseconds[3] += self;
				break;
			default:
				break;
			}
			// Follow parent identity rather than adding inclusive imagegraph spans:
			// nested imagegraph scopes otherwise charge the same work twice.
			size_t ancestor = index;
			for (size_t depth = 0; depth <= spans.size(); depth++) {
				const FrameSpan &candidate = spans[ancestor];
				if (candidate.Name.starts_with("imagegraph")) {
					if (span.Category != ProfileCategory::Idle) Milliseconds[4] += self;
					break;
				}
				if (candidate.Depth == 0 || candidate.Parent >= ancestor) break;
				ancestor = candidate.Parent;
			}
		}
	}

	uint64_t BenchmarkReport::FrameCount() const {
		return Frames;
	}

	bool BenchmarkReport::Write(
		const std::filesystem::path &path,
		double durationSeconds,
		double requiredSeconds,
		const HeapTotals &final,
		bool heapCompiled,
		std::span<const BenchmarkMetric> additional
	) const {
		if (!Valid || !heapCompiled || Frames == 0 || !std::isfinite(durationSeconds) ||
			!std::isfinite(requiredSeconds) || requiredSeconds <= 0.0 || durationSeconds < requiredSeconds ||
			final.TotalBytes < Initial.TotalBytes || final.TotalBlocks < Initial.TotalBlocks ||
			final.DroppedScopes < Initial.DroppedScopes || final.LiveBytes < 0 || final.LiveBlocks < 0 ||
			PeakBytes < 0)
			return false;
		for (double total : Milliseconds)
			if (!std::isfinite(total)) return false;
		for (double total : WallMilliseconds)
			if (!std::isfinite(total)) return false;
		for (const BenchmarkMetric &metric : additional) {
			if (metric.Name.empty() || !std::isfinite(metric.Value) || metric.Value < 0.0) return false;
			for (char character : metric.Name)
				if (!(character >= 'a' && character <= 'z') && !(character >= '0' && character <= '9') &&
					character != '_')
					return false;
		}
		std::ofstream output(path);
		if (!output) return false;
		output.imbue(std::locale::classic());
		output << std::setprecision(17)
			   << "{\n  \"schema\": 1,\n  \"heap_compiled\": true,\n  \"frame_count\": " << Frames
			   << ",\n  \"duration_seconds\": " << durationSeconds << ",\n  \"dropped_spans\": " << Dropped
			   << ",\n  \"off_thread_dropped_spans\": " << OffThreadDropped
			   << ",\n  \"owner_dropped_spans\": " << (Dropped - OffThreadDropped)
			   << ",\n  \"frame_basis\": \"update_iteration_or_server_tick\""
			   << ",\n  \"heap_dropped_scopes\": " << (final.DroppedScopes - Initial.DroppedScopes)
			   << ",\n  \"cpu_peak_basis\": \"frame_end_samples\",\n  \"gpu_peak_basis\": "
				  "\"process_lifetime\",\n"
			   << "  \"replication_timing_basis\": \"network_category_cpu_self\",\n  \"metrics\": {\n"
			   << "    \"cpu_live_bytes\": " << final.LiveBytes << ",\n    \"cpu_peak_bytes\": " << PeakBytes
			   << ",\n    \"cpu_process_peak_bytes\": " << final.PeakBytes
			   << ",\n    \"cpu_live_blocks\": " << final.LiveBlocks
			   << ",\n    \"cpu_profiler_overhead_bytes\": " << final.OverheadBytes
			   << ",\n    \"cpu_allocated_bytes\": " << final.TotalBytes - Initial.TotalBytes
			   << ",\n    \"cpu_allocations\": " << final.TotalBlocks - Initial.TotalBlocks
			   << ",\n    \"cpu_allocated_bytes_per_frame\": "
			   << static_cast<double>(final.TotalBytes - Initial.TotalBytes) / static_cast<double>(Frames)
			   << ",\n    \"cpu_allocated_bytes_per_second\": "
			   << static_cast<double>(final.TotalBytes - Initial.TotalBytes) / durationSeconds
			   << ",\n    \"cpu_allocations_per_second\": "
			   << static_cast<double>(final.TotalBlocks - Initial.TotalBlocks) / durationSeconds
			   << ",\n    \"cpu_allocations_per_frame\": "
			   << static_cast<double>(final.TotalBlocks - Initial.TotalBlocks) / static_cast<double>(Frames);
		constexpr std::array<std::string_view, 6> names = {
			"render_ms_per_frame",
			"physics_ms_per_frame",
			"replication_ms_per_frame",
			"script_ms_per_frame",
			"imagegraph_ms_per_frame",
			"frame_ms_per_frame"
		};
		for (size_t index = 0; index < names.size(); index++)
			output << ",\n    \"" << names[index]
				   << "\": " << Milliseconds[index] / static_cast<double>(Frames) << ",\n    \""
				   << names[index].substr(0, names[index].size() - 5)
				   << "second\": " << Milliseconds[index] / durationSeconds;
		constexpr std::array<std::string_view, 4> wallNames = {
			"render_wall_ms_per_frame",
			"physics_wall_ms_per_frame",
			"replication_wall_ms_per_frame",
			"imagegraph_wall_ms_per_frame"
		};
		for (size_t index = 0; index < wallNames.size(); index++) {
			output << ",\n    \"" << wallNames[index]
				   << "\": " << WallMilliseconds[index] / static_cast<double>(Frames) << ",\n    \""
				   << wallNames[index].substr(0, wallNames[index].size() - 5)
				   << "second\": " << WallMilliseconds[index] / durationSeconds;
		}
		for (const BenchmarkMetric &metric : additional)
			output << ",\n    \"" << metric.Name << "\": " << metric.Value;
		output << "\n  }\n}\n";
		output.close();
		return !output.fail();
	}
}
