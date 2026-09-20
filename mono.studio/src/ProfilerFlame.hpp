#pragma once

#include <studio/Diagnostics.hpp>
#include <vector>

namespace studio {
	// Worker reports follow the measured join as siblings under the same stage.
	// Return only direct world rows so nested systems do not double count.
	inline std::vector<DiagnosticSpan>
	AssignedJoinWorlds(std::span<const DiagnosticSpan> spans, size_t joinIndex) {
		std::vector<DiagnosticSpan> worlds;
		if (joinIndex >= spans.size() || spans[joinIndex].Name != "jobs.join.assigned") return worlds;
		const uint32_t stage = spans[joinIndex].Parent;
		if (stage >= spans.size()) return worlds;
		for (size_t index = joinIndex + 1; index < spans.size(); ++index) {
			const DiagnosticSpan &group = spans[index];
			if (group.Parent != stage || !group.Reported ||
				group.Name.find("pinned workers") == std::string::npos)
				continue;
			for (size_t child = index + 1; child < spans.size(); ++child) {
				if (spans[child].Depth <= group.Depth) break;
				if (spans[child].Parent == index && spans[child].Reported) worlds.push_back(spans[child]);
			}
			break;
		}
		return worlds;
	}

	inline void
	MeasuredProfilerSpans(std::span<const DiagnosticSpan> source, std::vector<DiagnosticSpan> &out) {
		out.clear();
		out.reserve(source.size());
		std::vector<uint32_t> retained(source.size(), engine::core::FrameGraph::NO_PARENT);
		for (size_t index = 0; index < source.size(); ++index) {
			if (source[index].Reported) continue;
			uint32_t parent = source[index].Parent;
			while (parent < index && retained[parent] == engine::core::FrameGraph::NO_PARENT)
				parent = source[parent].Parent;
			DiagnosticSpan copy = source[index];
			copy.Parent = parent < index ? retained[parent] : engine::core::FrameGraph::NO_PARENT;
			copy.Depth = copy.Parent == engine::core::FrameGraph::NO_PARENT ? 0 : out[copy.Parent].Depth + 1;
			retained[index] = static_cast<uint32_t>(out.size());
			out.push_back(std::move(copy));
		}
	}
}
