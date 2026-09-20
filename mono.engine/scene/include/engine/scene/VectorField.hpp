#pragma once

// Authored vector fields that any simulation may sample.
//
// A field is an ordinary placed instance. Its descendants select it as their
// source, which keeps the origin in the hierarchy instead of giving every
// consumer a second, fragile handle to it. The field shape itself is local when
// `LocalSpace` is set and world-aligned otherwise; either form can be bounded
// and can fade before its edge.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// A finite field over the local XZ plane. `Vector` is its constant term;
	// `Radial` and `Tangential` add position-dependent terms around its origin.
	struct VectorField2D {
		// Constant XZ vector added at every sampled point.
		core::Vector2 Vector;
		// Finite local XZ half-extent; zero on an axis leaves it unbounded.
		core::Vector2 HalfExtent;
		// Outward vector strength proportional to distance from the origin.
		float Radial = 0.0f;
		// Orbiting vector strength proportional to distance from the origin.
		float Tangential = 0.0f;
		// Positive edge-fade width in normalized field space.
		float Falloff = 0.0f;
		// Whether bounds and terms rotate with the field's transform.
		bool LocalSpace = true;
		// Explicit padding retained for component layout stability.
		uint8_t Reserved[3] = {};
	};

	// A finite field over all three local axes. `Axis` selects the orbit axis for
	// the tangential term and defaults to up, which makes an authored vortex a
	// normal field rather than a specialised effect.
	struct VectorField3D {
		// Constant three-dimensional vector added at every sampled point.
		core::Vector3 Vector;
		// Finite local half-extent; zero on an axis leaves it unbounded.
		core::Vector3 HalfExtent;
		// Normalized axis around which the tangential term orbits.
		core::Vector3 Axis{0.0f, 1.0f, 0.0f};
		// Outward vector strength proportional to radial distance.
		float Radial = 0.0f;
		// Orbiting vector strength around Axis.
		float Tangential = 0.0f;
		// Positive edge-fade width in normalized field space.
		float Falloff = 0.0f;
		// Whether bounds and terms rotate with the field transform.
		bool LocalSpace = true;
		// Explicit padding retained for component layout stability.
		uint8_t Reserved[3] = {};
	};

	// The compact, process-local result a consumer retains between hierarchy
	// refreshes. It contains no store pointer, so a particle block can sample it
	// for every particle without returning to ECS storage.
	struct VectorFieldSample {
		// World transform used to convert a sample point into field space.
		core::CFrame Frame;
		// Constant three-dimensional vector term.
		core::Vector3 Vector;
		// Resolved finite half-extent in field space.
		core::Vector3 HalfExtent;
		// Resolved orbit axis for the tangential term.
		core::Vector3 Axis{0.0f, 1.0f, 0.0f};
		// Entity that supplied this retained sample, or null when absent.
		ecs::Entity Source = ecs::NULL_ENTITY;
		// Resolved radial vector strength.
		float Radial = 0.0f;
		// Resolved tangential vector strength.
		float Tangential = 0.0f;
		// Resolved normalized edge-fade width.
		float Falloff = 0.0f;
		// Whether sampling uses the field's local orientation.
		bool LocalSpace = true;
		// Whether sampling ignores the local vertical axis.
		bool TwoDimensional = false;
	};

	// Finds the nearest vector-field ancestor, including `instance` itself.
	// Empty means the hierarchy selected no field and samples as zero.
	VectorFieldSample ResolveVectorField(const ecs::Store &store, ecs::Entity instance);

	// Samples a retained field at a world-space point. A zero half-extent on an
	// axis means that axis is unbounded. Outside a non-zero extent the field is
	// clamped away; a positive falloff fades it toward that boundary.
	core::Vector3 SampleVectorField(const VectorFieldSample &field, const core::Vector3 &worldPoint);
}
