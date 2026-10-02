#pragma once
#include <engine/assets/Mesh.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::bake {
	struct ComposerModelPart {
		std::string Material;
		std::vector<assets::MeshVertex> Vertices;
		std::optional<std::array<double, 16>> LocalMatrix;
	};
	struct ComposerModel {
		std::vector<ComposerModelPart> Parts;
		std::vector<std::array<std::array<float, 3>, 2>> Edges;
	};
	// Source-specific import. OBJ retains source centering, winding and flat normals;
	// element JSON retains named face materials and nested element transforms.
	bool ReadComposerObj(
		std::span<const std::byte> bytes,
		double scale,
		uint8_t axis,
		ComposerModel &out,
		std::string &failure,
		uint64_t maximumBytes = 64ull * 1024 * 1024
	);
	bool ReadComposerElementJson(
		std::span<const std::byte> bytes,
		double scale,
		uint8_t axis,
		ComposerModel &out,
		std::string &failure,
		uint64_t maximumBytes = 64ull * 1024 * 1024
	);
}
