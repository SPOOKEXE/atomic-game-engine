#pragma once

// A durable reference from one world sink to one authored image graph output.
//
// The graph document owns its node values and keyframes. This row names that
// document and the output to publish, so ECS does not keep a second graph copy
// beside the saved document.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Whether evaluation uses the authored tick or the world's fixed tick.
	//
	// @since v0.26
	enum class ImageGraphTickPolicy : uint8_t {
		// Evaluate at `ImageGraphBinding::FixedTick` for every update.
		Fixed,
		// Evaluate at the fixed simulation tick supplied by the owner.
		World,
	};

	// Sampling interpretation of generated RGBA8 bytes.
	enum class ImageGraphColorSpace : uint8_t { Display, Linear };

	// The maximum text length of each durable graph, output and texture selector.
	inline constexpr size_t IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES = 255;
	// Both stable tick-policy spellings currently use five bytes.
	inline constexpr uint32_t IMAGE_GRAPH_BINDING_TICK_POLICY_MAXIMUM_BYTES = 5;

	// The largest encoded value with three maximum-length selectors.
	inline constexpr uint32_t IMAGE_GRAPH_BINDING_MAXIMUM_SERIALISED_BYTES = static_cast<uint32_t>(
		sizeof(uint8_t) + 3 * (sizeof(uint32_t) + IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES) + sizeof(uint64_t) +
		sizeof(uint32_t) + IMAGE_GRAPH_BINDING_TICK_POLICY_MAXIMUM_BYTES + sizeof(uint64_t) +
		sizeof(uint32_t) + 7
	);

	// One authored output binding. The row contains references and evaluation
	// controls only; evaluated pixels and renderer handles belong to host adapters.
	//
	// @since v0.26
	struct ImageGraphBinding {
		// Deterministic random seed supplied to graph evaluation.
		uint64_t Seed = 0;
		// Evaluation tick used when `TickPolicy` is `Fixed`.
		uint64_t FixedTick = 0;
		// Durable name of the separately saved image graph document.
		core::Name Graph;
		// Durable identifier of the selected output in `Graph`.
		core::Name Output;
		// Name the renderer's texture owner publishes this output under.
		core::Name Texture;
		// Selected tick source. `FixedTick` remains authored when `World` is selected.
		ImageGraphTickPolicy TickPolicy = ImageGraphTickPolicy::Fixed;
		// Display is sRGB; linear is required for numeric PBR maps.
		ImageGraphColorSpace ColorSpace = ImageGraphColorSpace::Display;
		// Explicitly zeroed storage keeps the trivially-copyable row padding-free.
		std::array<uint8_t, 2> Reserved{};
	};

	// Checks required selectors, selector byte limits, tick policy and reserved bytes.
	bool IsValidImageGraphBinding(const ImageGraphBinding &binding);

	// Writes a checked authored binding to one live entity.
	//
	// RegisterSceneComponents must have run first. A rejected update leaves the row untouched.
	bool SetImageGraphBinding(ecs::Store &store, ecs::Entity entity, const ImageGraphBinding &binding);
}
