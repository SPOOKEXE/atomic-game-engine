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
		core::Vector2 Vector;
		core::Vector2 HalfExtent;
		float Radial = 0.0f;
		float Tangential = 0.0f;
		float Falloff = 0.0f;
		bool LocalSpace = true;
		uint8_t Reserved[3] = {};
	};

	// A finite field over all three local axes. `Axis` selects the orbit axis for
	// the tangential term and defaults to up, which makes an authored vortex a
	// normal field rather than a specialised effect.
	struct VectorField3D {
		core::Vector3 Vector;
		core::Vector3 HalfExtent;
		core::Vector3 Axis{0.0f, 1.0f, 0.0f};
		float Radial = 0.0f;
		float Tangential = 0.0f;
		float Falloff = 0.0f;
		bool LocalSpace = true;
		uint8_t Reserved[3] = {};
	};

	// The compact, process-local result a consumer retains between hierarchy
	// refreshes. It contains no store pointer, so a particle block can sample it
	// for every particle without returning to ECS storage.
	struct VectorFieldSample {
		core::CFrame Frame;
		core::Vector3 Vector;
		core::Vector3 HalfExtent;
		core::Vector3 Axis{0.0f, 1.0f, 0.0f};
		ecs::Entity Source = ecs::NULL_ENTITY;
		float Radial = 0.0f;
		float Tangential = 0.0f;
		float Falloff = 0.0f;
		bool LocalSpace = true;
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
