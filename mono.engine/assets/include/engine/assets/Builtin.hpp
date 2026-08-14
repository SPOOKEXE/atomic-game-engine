#pragma once

// Built-in meshes and textures are generated as ordinary `MeshData` and
// `TextureData`.
//
// **Generated rather than shipped as files**, which is the decision worth
// keeping. A checked-in `.amesh` of the same cube would be a second copy of
// geometry this file already describes, free to drift from it, needing a
// content store to reach and a fetch to arrive - and the whole point of a
// built-in is that it is there before any of that is. `resources/AGENTS.md`
// carries the same rule from the other side.
//
// @tier L8 · shared

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>

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

	// In-process selector; names cross persistence boundaries.
	//
	// @since v0.14
	enum class BuiltinTexture : uint8_t {
		// Pink and grey checkerboard - the sheet somebody puts on a wall while
		// they are still deciding where the wall goes.
		//
		// **A different thing from `render::MissingTexture` and it must stay
		// visibly different.** That marker is magenta and black and means "you
		// asked for a sheet that is not here"; this one is a texture an author
		// *chose*, and it means "this surface is deliberately untextured for
		// now". Draw them in the same colours and a typo becomes indistinguishable
		// from a decision, which is the exact failure `MissingTexture.hpp`
		// exists to prevent.
		Checker,
	};

	// Number of built-in textures.
	constexpr uint8_t BUILTIN_TEXTURE_COUNT = 1;

	// Stable, namespaced name for a built-in.
	//@{
	std::string_view BuiltinName(BuiltinMesh mesh);
	std::string_view BuiltinName(BuiltinTexture texture);
	//@}

	// Parses a built-in name; leaves `out` unchanged on failure.
	//@{
	bool BuiltinFromName(std::string_view name, BuiltinMesh &out);
	bool BuiltinFromName(std::string_view name, BuiltinTexture &out);
	//@}

	// Builds deterministic geometry with computed bounds.
	MeshData MakeBuiltin(BuiltinMesh mesh);

	// Builds the deterministic pixels of a built-in sheet.
	//
	// @param texture Which one.
	// @return An `RGBA8` image. Valid on every call.
	// @since v0.14
	TextureData MakeBuiltin(BuiltinTexture texture);
}
