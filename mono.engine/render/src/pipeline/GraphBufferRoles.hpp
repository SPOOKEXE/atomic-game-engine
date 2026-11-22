#pragma once

#include <engine/graph/RenderGraph.hpp>

#include <array>
#include <cstdint>

namespace engine::render::detail {

	enum GraphBufferRole : uint8_t {
		VertexBuffer = 1u << 0,
		IndexBuffer = 1u << 1,
		IndirectBuffer = 1u << 2,
	};

	// A resource's authored name does not determine how a draw consumes it.
	inline uint8_t GraphBufferRoles(const graph::RenderGraph &graph, graph::ResourceId resource) {
		uint8_t roles = 0;
		constexpr std::array defaults{VertexBuffer, IndexBuffer, IndirectBuffer};
		const std::array ports{core::Name("vertices"), core::Name("indices"), core::Name("commands")};
		for (uint32_t value = 1; value <= graph.Count(); ++value) {
			const auto *node = graph.Find(graph::NodeId{value});
			if (!node || !node->Enabled || node->Kind != core::Name("tessellated-draw")) continue;
			for (size_t index = 0; index < node->Reads.size(); ++index) {
				if (node->Reads[index] != resource) continue;
				if (index < node->ReadPorts.size()) {
					for (size_t port = 0; port < ports.size(); ++port)
						if (node->ReadPorts[index] == ports[port]) roles |= defaults[port];
				} else if (index < defaults.size())
					roles |= defaults[index];
			}
		}
		return roles;
	}
}
