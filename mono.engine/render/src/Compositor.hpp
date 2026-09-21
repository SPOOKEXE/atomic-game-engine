#pragma once

// CPU packing for the small authored settings shared by compositor shaders.

#include <engine/graph/RenderGraph.hpp>

#include <glm/vec4.hpp>

#include <array>

namespace engine::render {

	using CompositorParameters = std::array<glm::vec4, 3>;

	// Packs one built-in compositor node into the fixed shader uniform tail.
	CompositorParameters CompositorParametersFor(const graph::Node &node);
}
