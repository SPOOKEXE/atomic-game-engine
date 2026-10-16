#pragma once

// Audio's closed node vocabulary. The graph consumes these declarations when
// admitting kinds and wires; the device-facing node state stays in Graph.hpp.

#include <engine/audio/Graph.hpp>
#include <engine/graphcore/Schema.hpp>

#include <span>

namespace engine::audio::detail {

	struct RoutingPorts {
		const graphcore::PortSchema *Output = nullptr;
		const graphcore::PortSchema *Input = nullptr;
	};

	// Initializing this table validates it. AudioGraph calls it during setup,
	// before a device callback may change routing.
	std::span<const graphcore::NodeSchema> NodeSchemas();
	const graphcore::NodeSchema *NodeSchemaFor(NodeKind kind);
	RoutingPorts RouteFor(NodeKind from, NodeKind to);
}
