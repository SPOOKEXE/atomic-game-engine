#pragma once

// Image operation declarations shared by compilation and authoring adapters.
// Payloads, defaults and pixel evaluation remain owned by imagegraph.
// @tier L9 · shared

#include <engine/graphcore/Schema.hpp>

#include <array>
#include <span>
#include <string_view>

namespace engine::imagegraph {

	// Preserves the image-port identity used by existing Composer projects.
	inline constexpr std::string_view IMAGE_PORT_TYPE = "composer.image";

	// Durable kind names in Operation variant order, without allocating on lookup.
	inline constexpr std::array<std::string_view, 7> NODE_KIND_IDS{
		"image.source",
		"image.solid",
		"image.resize",
		"image.crop",
		"image.transform",
		"image.flip",
		"image.blend"
	};

	// Immutable declarations for the supported image operations and their named pins.
	std::span<const graphcore::NodeSchema> NodeSchemas();

	// Borrows a supported operation schema, or returns null for an unknown kind.
	const graphcore::NodeSchema *FindNodeSchema(std::string_view kind);
}
