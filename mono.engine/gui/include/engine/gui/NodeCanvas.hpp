#pragma once

// The saveable graph relations behind the NodeCanvas GUI objects.
//
// Drawing a node graph and deciding whether its graph is legal are separate
// jobs. This file owns the latter, so a headless host can validate the same
// graph a client renders without learning about a device or a script VM.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::core {
	class Name;
}

namespace engine::gui {

	// Why a request to make a wire was refused.
	enum class NodeLinkResult : uint8_t {
		Made,
		NotInCanvas,
		NotAPort,
		WrongDirection,
		MissingId,
		TypeMismatch,
		InvalidConnectionLimit,
		InputFull,
		WouldCycle,
	};

	// Why a configured pass-through mapping cannot be used.
	enum class NodeBypassResult : uint8_t {
		Valid,
		NotANode,
		NotBypassing,
		MissingPort,
		WrongDirection,
		TypeMismatch,
	};

	// Explains a link result in text suitable for a script error or a tool tip.
	const char *Describe(NodeLinkResult result);

	// Explains a bypass mapping result in text suitable for a script error or a
	// tool tip.
	const char *Describe(NodeBypassResult result);

	// Returns whether an output type may feed an input type. `any` is accepted at
	// either end, while every other type must be the same stable type name.
	bool NodeCanvasTypesCompatible(const core::Name &output, const core::Name &input);

	// Validates the input and output a bypassing node declares directly beneath
	// it. This is structural validation only: a graph runtime still validates an
	// `any` source against its value at evaluation time.
	NodeBypassResult ValidateNodeCanvasBypass(const ecs::Store &store, ecs::Entity node);

	// Connects an output port to an input port under one `NodeCanvas`.
	//
	// `MaxConnections == 0` accepts every wire. Negative limits are refused. A one-wire input replaces its
	// old wire, exactly as dropping a new cable onto a socket does in a node editor; higher finite limits
	// refuse a wire once full.
	//
	// @param store  The graph's world.
	// @param canvas The NodeCanvas owning both ports.
	// @param output The output NodeCanvasPort.
	// @param input  The input NodeCanvasPort.
	// @param link   Filled with the new NodeCanvasLink instance on success.
	// @return Why the request was made or refused.
	NodeLinkResult ConnectNodePorts(
		ecs::Store &store, ecs::Entity canvas, ecs::Entity output, ecs::Entity input, ecs::Entity &link
	);

	// Removes the wire ending at `input` in `canvas`.
	//
	// @return `true` when a connection was removed.
	bool DisconnectNodeInput(ecs::Store &store, ecs::Entity canvas, ecs::Entity input);

	// Finds every wire instance owned by `canvas`.
	//
	// @param store  The graph's world.
	// @param canvas The NodeCanvas to inspect.
	// @param out    Cleared and filled in tree order.
	// @return The number of links found.
	size_t NodeCanvasLinks(const ecs::Store &store, ecs::Entity canvas, std::vector<ecs::Entity> &out);

	// Updates the non-manual groups directly below `canvas` from their direct
	// node children. Nodes and groups must use offset-only positions and sizes.
	//
	// The operation preserves each node's absolute location while moving it into
	// the group's local space. It returns how many groups changed.
	size_t LayoutNodeCanvasGroups(ecs::Store &store, ecs::Entity canvas);

	// Updates the non-manual input-port layout on every node below `canvas`.
	// Nodes and ports must use offset-only sizes and top-left anchors.
	//
	// `Separate` distributes each edge's ports, and `Squash` packs them from its
	// first corner. The return value is the number of ports repositioned.
	size_t LayoutNodeCanvasPorts(ecs::Store &store, ecs::Entity canvas);
}
