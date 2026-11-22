#pragma once

// The local light cap and one resolved local light.

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::render {
	// How many local lights one frame may carry.
	//
	// **This is the one home of the number, and `opaque.frag` is told it.**
	// `mono.engine/resources/CMakeLists.txt` reads the value out of this
	// declaration and passes `-DMAX_LIGHTS` to glslc, so the shader's loop bound
	// and the uniform buffer this sizes cannot disagree - there is no second
	// literal left to drift. Changing it here changes both.
	//
	// It was spelled twice until v0.10 and nothing checked it, which `AGENTS.md`
	// rule 6 calls documentation rather than a constraint. The reason it is
	// arranged so a mismatch is impossible, rather than tested for, is that it
	// could not be tested for: what gets staged is SPIR-V, and the constant is
	// folded away by then.
	//
	// A mismatch was never a validation error. It is a light set that silently
	// reads past its own count or stops short of it - which looks like one lamp
	// not working.
	//
	// **Keep this a plain integer literal on one line.** The configure step
	// matches it with a regex and fails loudly if it cannot, so an expression
	// here is a configure error rather than a silent revert to a stale value.
	//
	// @since v0.10
	inline constexpr size_t MAX_SCENE_LIGHTS = 16;

	// One local light, resolved into world space.
	//
	// **Resolved rather than an entity, for `SurfaceView`'s reason**: this is what
	// the device layer takes, and a world's identifier in it would be a world's
	// business leaking into a pipeline. `scene::Light` says what a light *is*;
	// where it shines from is its parent's, and the client walks that.
	//
	// @since v0.10
	struct SceneLight {
		// Where it is, in world space.
		core::Vector3 Position;

		// How far it reaches, in metres. Past this it contributes nothing, which
		// is what lets a fragment reject it with one compare.
		float Range = 8.0f;

		// Its colour, already multiplied by brightness.
		//
		// **Multiplied here rather than in the shader**, because brightness is a
		// scalar an author sets and the shader wants a colour - folding them at
		// the boundary is one multiply per light per frame against one per light
		// per *fragment*.
		core::Color3 Colour{1.0f, 1.0f, 1.0f};

		// Which way a spot points. Ignored when `ConeCosine` is -1.
		core::Vector3 Direction{0.0f, -1.0f, 0.0f};

		// The cosine of the cone's half-angle, or -1 for a point light.
		//
		// **A cosine and not an angle**, because the test is a dot product: an
		// angle would be an `acos` per light per fragment to compare something the
		// dot product already gives.
		float ConeCosine = -1.0f;

		// The authored request for a depth-tested local-light term. The renderer
		// retains this alongside the resolved row so final lighting and capture
		// make the same availability decision.
		bool Shadows = false;

		// Interned runtime identity of the authored DataFactoryId, when present.
		core::Name DataFactoryId = {};
	};
}
