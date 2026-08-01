#pragma once

// The built-in meshes.
//
// Public rather than tucked into Renderer.cpp so that the winding can be
// tested. A face wound the wrong way is invisible from outside and visible from
// inside, which does not look like a winding bug — it looks like the renderer
// is dropping triangles at random as the object turns.
//
// @tier L12 · client

#include <array>
#include <cstdint>

namespace engine::render {

	// One object-space vertex for the built-in opaque mesh pipeline.
	//
	// @client
	struct MeshVertex {
		// Object-space XYZ position consumed by the model transform.
		float Position[3];

		// Object-space XYZ unit normal.
		float Normal[3];
	};

	// Half of the unit cube's side length, in object-space units.
	//
	// @client
	inline constexpr float CUBE_HALF_EXTENT = 0.5f;

	// Twenty-four object-space vertices for a unit cube centred on the origin.
	//
	// Each face owns four vertices so its normal stays flat instead of being
	// averaged across a shared corner.
	//
	// @client
	inline constexpr std::array<MeshVertex, 24> CUBE_VERTICES{{
		// +X
		{{0.5f, -0.5f, -0.5f}, {1, 0, 0}},
		{{0.5f, 0.5f, -0.5f}, {1, 0, 0}},
		{{0.5f, 0.5f, 0.5f}, {1, 0, 0}},
		{{0.5f, -0.5f, 0.5f}, {1, 0, 0}},
		// -X
		{{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}},
		{{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}},
		{{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}},
		{{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}},
		// +Y
		{{-0.5f, 0.5f, -0.5f}, {0, 1, 0}},
		{{-0.5f, 0.5f, 0.5f}, {0, 1, 0}},
		{{0.5f, 0.5f, 0.5f}, {0, 1, 0}},
		{{0.5f, 0.5f, -0.5f}, {0, 1, 0}},
		// -Y
		{{-0.5f, -0.5f, 0.5f}, {0, -1, 0}},
		{{-0.5f, -0.5f, -0.5f}, {0, -1, 0}},
		{{0.5f, -0.5f, -0.5f}, {0, -1, 0}},
		{{0.5f, -0.5f, 0.5f}, {0, -1, 0}},
		// +Z
		{{-0.5f, -0.5f, 0.5f}, {0, 0, 1}},
		{{0.5f, -0.5f, 0.5f}, {0, 0, 1}},
		{{0.5f, 0.5f, 0.5f}, {0, 0, 1}},
		{{-0.5f, 0.5f, 0.5f}, {0, 0, 1}},
		// -Z
		{{0.5f, -0.5f, -0.5f}, {0, 0, -1}},
		{{-0.5f, -0.5f, -0.5f}, {0, 0, -1}},
		{{-0.5f, 0.5f, -0.5f}, {0, 0, -1}},
		{{0.5f, 0.5f, -0.5f}, {0, 0, -1}},
	}};

	// Thirty-six 16-bit indices forming two counter-clockwise triangles per face.
	//
	// Every quad uses order `0-1-2, 0-2-3`, seen from outside the cube, matching
	// the pipeline's `FRONTFACE_COUNTER_CLOCKWISE` and `CULLMODE_BACK`. For every
	// triangle, `(v1 - v0) x (v2 - v0)` points with the face normal;
	// `tests/Primitives.cpp` checks all twelve.
	//
	// @client
	inline constexpr std::array<uint16_t, 36> CUBE_INDICES{{
		0,	1,	2,	0,	2,	3,	// +X
		4,	5,	6,	4,	6,	7,	// -X
		8,	9,	10, 8,	10, 11, // +Y
		12, 13, 14, 12, 14, 15, // -Y
		16, 17, 18, 16, 18, 19, // +Z
		20, 21, 22, 20, 22, 23, // -Z
	}};
}
