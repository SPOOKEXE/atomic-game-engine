#pragma once

// The world's resolved lighting, and where it comes from.
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
// `Lighting` is the authored source. `LightingOf` resolves its clock and
// latitude into one directional light, then carries the ambient and fog terms
// without making the renderer search an instance tree. A legacy `Sun` resource
// remains an explicit C++ override for the direction and ambient term.
//
// @tier L7 · shared

#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/ShaderLens.hpp>
#include <engine/scene/Volume.hpp>

#include <array>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// A pixel samples every active entry, so the cap is renderer work rather
	// than an arbitrary hierarchy restriction. Additional authored volumes stay
	// valid scene data and are simply not selected for this presentation frame.
	inline constexpr size_t MAX_SCENE_VOLUMES = 4;

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

	// Everything a world contributes to its built-in lighting model.
	//
	// Values are resolved from the `Lighting` service once per presented world.
	// The renderer receives this copy, so no pointer crosses the world boundary
	// and recursive portal views can reuse the same authored state with their
	// own eye position.
	//
	// @since v0.16
	struct WorldLighting {
		// Which way the sun shines towards, as a unit vector.
		core::Vector3 Direction = SUN_DIRECTION;

		// Light present on every face.
		core::Color3 Ambient = SUN_AMBIENT;

		// Sky light applied according to how far a face points upward.
		core::Color3 OutdoorAmbient{};

		// The directional contribution after `Brightness` and daylight are
		// applied.
		core::Color3 Direct{1.0f, 1.0f, 1.0f};

		// What distant geometry fades towards.
		core::Color3 FogColor{0.05f, 0.06f, 0.09f};

		// Where the distance fade starts and becomes complete, in metres.
		//@{
		float FogStart = 100000.0f;
		float FogEnd = 100001.0f;
		//@}

		// The first skybox, atmosphere and cloud providers beneath `Lighting`.
		// This is one resolved value rather than parallel renderer state, so a
		// redraw signature and a render pass cannot select different siblings.
		//
		// @since v0.19
		Environment EnvironmentState;

		// The bounded participating-media snapshot. The values were resolved from
		// placed `Volume` instances while the world was entered and are safe to
		// retain across the renderer boundary.
		std::array<VolumeState, MAX_SCENE_VOLUMES> Volumes{};
		size_t VolumeCount = 0;

		// World-space screen effects selected while the world is entered. As with
		// volumes, this remains authored data until it is copied for presentation.
		std::array<ShaderLensState, MAX_SCENE_SHADER_LENSES> ShaderLenses{};
		size_t ShaderLensCount = 0;
	};

	// Resolves the `Lighting` service into the values a renderer consumes.
	//
	// The solar arc is an equinox arc. `ClockTime` supplies the hour angle and
	// `GeographicLatitude` supplies the noon elevation; a date is deliberately
	// not invented when the service carries none.
	//
	// @param store The world.
	// @return Its resolved lighting, or the legacy defaults when it has no
	//         `Lighting` service.
	// @since v0.16
	WorldLighting LightingOf(const ecs::Store &store);

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
