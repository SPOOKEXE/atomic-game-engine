#pragma once

#include <engine/core/FrameGraph.hpp>

#include <algorithm>
#include <studio/Diagnostics.hpp>
#include <vector>

namespace studio {
	inline void
	PhysicsProfilerSpanMask(std::span<const engine::core::FrameSpan> spans, std::vector<bool> &include) {
		include.assign(spans.size(), false);
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &span = spans[index];
			if (span.Category != engine::core::ProfileCategory::Physics &&
				!(span.Reported && span.Name.starts_with("physics.")))
				continue;
			for (uint32_t parent = static_cast<uint32_t>(index); parent < spans.size();) {
				include[parent] = true;
				const uint32_t next = spans[parent].Parent;
				if (next >= parent) break;
				parent = next;
			}
		}
	}

	// Copies one completed frame that contains physics work. Frames between
	// fixed physics ticks leave the previous reading intact, so a faster Studio
	// presentation rate cannot erase the last solver result.
	inline bool CapturePhysicsProfilerSpans(
		std::span<const engine::core::FrameSpan> spans, std::vector<DiagnosticSpan> &into
	) {
		std::vector<bool> include;
		PhysicsProfilerSpanMask(spans, include);
		if (std::find(include.begin(), include.end(), true) == include.end()) return false;

		into.clear();
		into.reserve(spans.size());
		std::vector<uint32_t> kept(spans.size(), engine::core::FrameGraph::NO_PARENT);
		for (size_t index = 0; index < spans.size(); ++index) {
			const engine::core::FrameSpan &source = spans[index];
			if (!include[index]) continue;
			uint32_t parent = source.Parent;
			while (parent < index && kept[parent] == engine::core::FrameGraph::NO_PARENT)
				parent = spans[parent].Parent;
			const uint32_t retainedParent =
				parent < index ? kept[parent] : engine::core::FrameGraph::NO_PARENT;
			into.push_back(
				DiagnosticSpan{
					.Name = std::string(source.Name),
					.Depth = retainedParent == engine::core::FrameGraph::NO_PARENT
								 ? 0
								 : into[retainedParent].Depth + 1,
					.Parent = retainedParent,
					.StartMilliseconds = source.StartMilliseconds,
					.Milliseconds = source.Milliseconds,
					.SelfMilliseconds = source.SelfMilliseconds,
					.IdleMilliseconds = source.IdleMilliseconds,
					.Category = source.Category,
					.Owner = source.Owner,
					.Reported = source.Reported
				}
			);
			kept[index] = static_cast<uint32_t>(into.size() - 1);
		}
		return true;
	}
}
