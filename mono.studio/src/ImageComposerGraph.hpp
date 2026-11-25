#pragma once

#include <engine/ang/Graph.hpp>
#include <engine/ang/Registry.hpp>

#include <string_view>

namespace studio {
	// The composer's node types: one per image kind, its properties as params
	// with widget hints. Studio owns it; nothing registers into a global table.
	const engine::ang::Registry &ImageComposerTypes();

	// A node's value for one property, or its definition default.
	engine::ang::Value ImageComposerParam(const engine::ang::NodeInstance &node, std::string_view key);
}
