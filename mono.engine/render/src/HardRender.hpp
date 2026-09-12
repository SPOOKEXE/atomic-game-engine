#pragma once

// Shared CPU contract for the built-in hard compute nodes.

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <glm/vec4.hpp>

namespace engine::render {

	struct TraceOptions {
		glm::vec4 Trace{32.0f, 100.0f, 0.1f, 1.0f};
		glm::uvec4 Inputs{};
	};

	inline bool IsTraceNode(core::Name kind) {
		return kind == core::Name("global-illumination") || kind == core::Name("raytrace") ||
			   kind == core::Name("pathtrace");
	}

	inline TraceOptions TraceOptionsFor(const graph::Node &node, bool historyAvailable) {
		TraceOptions options;
		if (node.Kind == core::Name("global-illumination")) {
			options.Trace.x = node.Number(core::Name("rays-per-pixel"), 1.0f);
			options.Trace.y = node.Number(core::Name("max-distance"), 24.0f);
		} else if (node.Kind == core::Name("pathtrace")) {
			options.Trace.x = node.Number(core::Name("max-bounces"), 3.0f);
			options.Trace.w = node.Number(core::Name("samples-per-frame"), 1.0f);
		} else {
			options.Trace.x = node.Number(core::Name("steps"), 32.0f);
			options.Trace.y = node.Number(core::Name("max-distance"), 100.0f);
			options.Trace.z = node.Number(core::Name("thickness"), 0.1f);
		}
		options.Inputs.x = historyAvailable ? 1u : 0u;
		return options;
	}
}
