#pragma once

#include <studio/Diagnostics.hpp>
#include <vector>

namespace studio {
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
