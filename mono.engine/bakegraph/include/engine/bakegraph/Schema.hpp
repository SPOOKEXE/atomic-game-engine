#pragma once

// Pure port declarations for the existing bake node kinds. Runtime payloads,
// decoders, parameters and evaluation stay in Engine::bake.
// @tier L9 · shared

#include <engine/bakegraph/Nodes.hpp>
#include <engine/graphcore/Schema.hpp>

#include <span>

namespace engine::bake {

	// Every declared bake kind in its existing ordinal order. Schema identities
	// describe the kinds but never replace NodeKind in a saved document.
	std::span<const graphcore::NodeSchema> NodeSchemas();

	// The pure declaration for one closed kind, or null for an unknown ordinal.
	const graphcore::NodeSchema *NodeSchemaFor(NodeKind kind);
}
