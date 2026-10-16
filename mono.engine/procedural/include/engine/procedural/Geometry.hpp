#pragma once

// Bounded, object-local triangle geometry for generated regional content.
// Asset and live-scene adapters consume the same pure output.
// @tier L3 · shared

#include <engine/procedural/Region.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::procedural {

	// Object-local vertex on an X/Z ground plane with Y as height.
	struct GeometryVertex {
		std::array<float, 3> Position{}; // Local cell units relative to GeometryObject::Origin.
		std::array<float, 3> Normal{};	 // Outward unit normal.
		std::array<float, 2> UV{};		 // Local texture coordinate.
	};

	// One independently addressable generated object. Kind is stable text:
	// "tree", "village-lot", "house", "entrance", or "road".
	struct GeometryObject {
		std::string Id{};						// Original stable placement or road identity.
		std::string Kind{};						// Named geometry category.
		Point Origin{};							// Exact global fixed-point X/Z origin.
		uint16_t Material = 0;					// Generator material choice; roads use zero.
		uint16_t Variant = 0;					// Generator variant choice; roads use zero.
		std::vector<GeometryVertex> Vertices{}; // Object-local triangle vertices.
		std::vector<uint32_t> Indices{};		// Three indices per triangle.
	};

	// Complete geometry transaction, ordered by stable object identity.
	struct RegionGeometry {
		std::vector<GeometryObject> Objects{};
		uint32_t VertexCount = 0; // Total vertices across objects.
		uint32_t IndexCount = 0;  // Total triangle indices across objects.
	};

	// Hard ceilings checked before any output geometry is allocated.
	struct GeometryLimits {
		static constexpr uint32_t HARD_MAXIMUM_OBJECTS = 4352;
		static constexpr uint32_t HARD_MAXIMUM_VERTICES = 500000;
		static constexpr uint32_t HARD_MAXIMUM_INDICES = 500000;
		static constexpr size_t HARD_MAXIMUM_BYTES = 20 * 1024 * 1024;
		uint32_t MaximumObjects = HARD_MAXIMUM_OBJECTS;	  // Combined placements and roads.
		uint32_t MaximumVertices = HARD_MAXIMUM_VERTICES; // Combined object-local vertices.
		uint32_t MaximumIndices = HARD_MAXIMUM_INDICES;	  // Combined triangle indices.
		size_t MaximumBytes = HARD_MAXIMUM_BYTES;		  // Estimated owned output storage.
	};

	// Converts a canonical region result into visible trees, lots, houses,
	// entrance markers, and quadratic road ribbons. Scope validates ownership
	// and bounds coordinate subtraction; all geometry stays local to Origin.
	// Refusal leaves out untouched. No world, asset, file, or device is touched.
	Status BuildRegionGeometry(
		const RegionResult &result,
		RegionScope scope,
		RegionGeometry &out,
		Diagnostic &diagnostic,
		const GeometryLimits &limits = {}
	);
}
