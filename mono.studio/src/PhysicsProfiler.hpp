#pragma once

#include <engine/core/FrameGraph.hpp>

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
}
