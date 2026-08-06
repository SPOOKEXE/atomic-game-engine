#pragma once

// Built-in meshes are generated as ordinary unit `MeshData`.
// @tier L8 · shared

#include <engine/assets/Mesh.hpp>

#include <cstdint>
#include <string_view>

namespace engine::assets {

	// In-process selector; names cross persistence boundaries.
	enum class BuiltinMesh : uint8_t {
		// Unit cube.
		Cube,

		// Quad on XZ facing +Y.
		Plane,

		// Wedge sloping along Z.
		Wedge,

		// Wedge sloping along two axes.
		CornerWedge,

		// UV sphere of radius 0.5 with smooth normals.
		Sphere,

		// Cylinder of radius 0.5 about Y.
		Cylinder,
	};

	// Number of built-ins.
	constexpr uint8_t BUILTIN_MESH_COUNT = 6;

	// Stable, namespaced name for a built-in.
	std::string_view BuiltinName(BuiltinMesh mesh);

	// Parses a built-in name; leaves `out` unchanged on failure.
	bool BuiltinFromName(std::string_view name, BuiltinMesh &out);

	// Builds deterministic geometry with computed bounds.
	MeshData MakeBuiltin(BuiltinMesh mesh);
}
