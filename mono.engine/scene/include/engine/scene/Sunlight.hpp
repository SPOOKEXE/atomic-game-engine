#pragma once

// The world's one directional light, and where it comes from.
//
// **Here rather than in `render`, because something other than the renderer
// needs it now.** The direction lived as a `constexpr` in `Renderer.cpp`, which
// was right while the only thing that could ask was the pass that shades with
// it. A hole changed that: the far half of a body standing in a portal is a
// *copy* of the near half turned by the seam's rotation, and a copy turned by
// `R` has to be lit by `R · L` or its two halves are shaded by two suns that
// differ by exactly that turn. `scene::CutAndCloneSeams` is what maps it, and it
// cannot reach a constant in the renderer's anonymous namespace.
//
// **A resource, because it is authored** - `scene::Gravity` is the same idea and
// says why: a world underground, at night, or seen from above wants a different
// vector, and one that wants none turns the brightness down rather than fighting
// a constant. A world that never sets one gets `SUN_DIRECTION` and
// `SUN_AMBIENT`, which are the numbers this engine has always drawn with, so
// nothing that existed before this looks any different.
//
// **What it deliberately is not is `Lighting.ClockTime`.** That service already
// carries a clock and a latitude, and Roblox derives a sun arc from the pair -
// which is the right feature and a different one: it would move the light in
// every scene that has ever been authored against a fixed vector. When it
// arrives it writes this resource, and everything downstream is unchanged.
//
// @tier L7 · shared

#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Where the sun shines *towards*, as a unit vector.
	//
	// **Towards and not from**, matching `opaque.frag`, which negates it to get
	// the direction to the light. Down and across, so a box has a lit face, a
	// shaded face and a shadow with a length to it - a light straight overhead
	// gives every upright surface the same tone and reads as no lighting at all.
	inline constexpr core::Vector3 SUN_DIRECTION{-0.45f, -0.8f, -0.4f};

	// What reaches a surface the sun does not.
	//
	// **Blue, and not grey.** Ambient stands in for the sky here, and a sky is
	// blue; a neutral ambient makes a shadowed face read as a darker copy of a
	// lit one rather than as a face in shade.
	inline constexpr core::Color3 SUN_AMBIENT{0.26f, 0.28f, 0.34f};

	// The world's directional light.
	//
	// @since v0.15
	struct Sun {
		// Which way it shines, as a unit vector. Normalised on read rather than
		// on write, so a world may set an unnormalised one and mean it.
		core::Vector3 Direction = SUN_DIRECTION;

		// What reaches a surface it does not.
		core::Color3 Ambient = SUN_AMBIENT;
	};

	// The world's sun, or the default when it has never set one.
	//
	// **A free function rather than `store.Resource<Sun>()` at every call
	// site**, because "no sun resource" and "the default sun" have to be the
	// same answer: a world loaded from a file that predates this must light the
	// way it always did, and a caller that forgot the null check would light it
	// black.
	//
	// @param store The world.
	// @return Its sun, with `Direction` normalised.
	// @since v0.15
	Sun SunOf(const ecs::Store &store);
}
