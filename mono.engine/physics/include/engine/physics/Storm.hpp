#pragma once

// Fixed-step rigid-body coupling for scene's analytical storm field.
//
// @tier L8 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/scene/Storm.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// One authored storm resource. It is absent until explicitly installed, so
	// ordinary worlds do not acquire weather they did not ask for.
	struct Storm {
		scene::StormState State;  // Authored field and fixed-tick trajectory.
		bool Enabled = true;	  // Whether physics samples and advances this resource.
		uint8_t Reserved[3] = {}; // Explicit initialized serialization padding.
	};

	// Per-body aerodynamic response. Area zero derives the broadest projected
	// collider face, which keeps simple parts useful without a second size field.
	struct StormResponse {
		float ExposedArea = 0.0f;	  // Square metres, or zero to derive collider area.
		float DragCoefficient = 1.0f; // Dimensionless aerodynamic drag coefficient.
		float ForceScale = 1.0f;	  // Authored multiplier after the drag calculation.
		bool Enabled = true;		  // Whether this body accepts storm aerodynamic force.
		uint8_t Reserved[3] = {};	  // Explicit initialized serialization padding.
	};

	// Per-link failure data. MaterialStrength is authored by the material layer:
	// a link's own break rating remains separate from what it is made from.
	struct StormLink {
		float BreakForce = 0.0f;	   // Newtons before material scaling, zero means unbreakable.
		float MaterialStrength = 1.0f; // Material-specific strength multiplier.
		float Integrity = 1.0f;		   // Remaining fatigue capacity, depleted by sustained overload.
		float DamageRate = 1.0f;	   // Fatigue accumulated per overloaded second.
		bool Enabled = true;		   // Whether wind may fail this link.
		uint8_t Reserved[3] = {};	   // Explicit initialized serialization padding.
	};

	// Fixed-base vegetation that flexes toward the sampled horizontal wind. RestFrame
	// is authored state, while the current bend is replicated simulation state.
	struct StormVegetation {
		core::CFrame RestFrame;					// Unbent authored pose.
		float BendRadians = 0.0f;				// Current lean from RestFrame.
		float BendDirectionRadians = 0.0f;		// World-space horizontal lean direction.
		float MaximumBendRadians = 0.82f;		// Largest permitted lean.
		float ResponsePerSecond = 4.5f;			// Rate at which the stem follows wind.
		float WindSpeedForMaximumBend = 115.0f; // Horizontal speed that reaches the limit.
		bool Enabled = true;					// Whether this object flexes in storm wind.
		uint8_t Reserved[3] = {};				// Explicit initialized serialization padding.
	};

	// Registers storm resource and response components under stable names.
	void RegisterStormComponents();

	// Installs or replaces the world's authored storm state.
	void SetStorm(ecs::Store &store, const Storm &storm);

	// Removes the storm resource. Bodies retain their authored responses.
	void ClearStorm(ecs::Store &store);

	// Returns the installed storm, or null when this world has none.
	const Storm *StormOf(const ecs::Store &store);

	// Advances the storm and applies one aerodynamic fixed-step load. A linked
	// WeldConstraint or legacy Weld with StormLink fails before its bodies are
	// accelerated; the existing rigid-joint solver then rebuilds connectivity.
	void ApplyStormForces(ecs::Store &store);
}
