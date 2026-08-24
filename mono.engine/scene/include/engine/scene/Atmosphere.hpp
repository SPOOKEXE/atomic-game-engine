#pragma once

// The sky between the eye and everything else, and the sky above it.
//
// **There is no `Fog` type here, and its absence is the decision.**
// `docs/ARCH_REVIEW.md` D4 reads the tree as having no fog at all; it has, in
// the one place it belongs. `LightingServiceComponent::FogColor`, `FogStart` and
// `FogEnd` are authored on the `Lighting` service, `LightingOf` resolves them
// into `WorldLighting`, and `render::ViewRecording` puts them in a uniform. A
// `scene::Fog` component would be a second answer to what a world's distance
// fade is, which is rule 2 and is exactly the mistake `Humanoid::Radius` was
// deleted for.
//
// **`Atmosphere` is separate because linear fog cannot express it.** Two
// distances and a colour give a fade that is the same looking up as looking
// along the ground; scattering is not. Roblox draws the same line:
// `Atmosphere` is an instance under `Lighting`, while the legacy fog remains
// three properties on that service.
//
// **Both of these are per-world presentation state and neither reaches a
// simulation input.** That is decision 20: "render graphs may vary per platform.
// Anything reaching a simulation input may not." So nothing here is read by
// physics, by a character controller or by a query, and a host that drew the sky
// differently would still simulate the same world. The test to apply to a field
// somebody wants to add: if a body's trajectory would change, it does not belong
// here.
//
// **A component on an instance under `Lighting`, which is `Sun`'s neighbour
// rather than its copy.** `scene::Sun` is a resource because it is a C++
// override with no instance behind it and `Sunlight.hpp` says so; an atmosphere
// is authored content that an author adds and removes, so it is an instance -
// the same call `Material` makes against being a property on `BasePart`.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Classes.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// How thick the air is, on an `Atmosphere` instance under `Lighting`.
	//
	// Roblox's `Atmosphere`, field for field, because the names are the ones an
	// author has already learned and §8.1 says the ubiquitous language here is
	// Roblox's deliberately.
	//
	// @since v0.19
	struct Atmosphere {
		// What the air scatters, which is what distance fades towards.
		//
		// **The colour of the medium and not of the sky.** A sky is what the
		// medium looks like when there is nothing in front of it, so it falls out
		// of this and `Decay` rather than being a fourth field to keep in step.
		core::Color3 Colour{0.78f, 0.83f, 0.87f};

		// What the air removes from what passes through it.
		//
		// Roblox's `Atmosphere.Decay`. A warm decay against a cool colour is what
		// makes a sunset read as a sunset rather than as an orange filter.
		core::Color3 Decay{0.42f, 0.48f, 0.55f};

		// How much air there is between the eye and everything, 0 to 1.
		//
		// **The one dial that is not a colour**, and the one an author reaches
		// for: zero is vacuum and one is the thickest this model goes. A world
		// that sets nothing else and raises this gets haze.
		float Density = 0.395f;

		// How far the effect is pushed away from the eye, -1 to 1.
		//
		// Roblox's `Atmosphere.Offset`. Negative brings the haze onto near
		// geometry, which is what an indoor scene wants; positive holds it off
		// until the middle distance.
		float Offset = 0.0f;

		// How much the sun blooms through the air, 0 to 10.
		float Glare = 0.0f;

		// How much the air whitens near the horizon, 0 to 10.
		float Haze = 0.0f;
	};

	// A cloud layer, on a `Clouds` instance under the atmosphere.
	//
	// **A layer and not a field on `Atmosphere`**, because `ROADMAP.md` v0.22
	// lists them separately and they are separable: a world can have haze and no
	// clouds, and a world can have two layers moving at different speeds. Roblox's
	// `Clouds` is an instance under `Terrain` rather than under `Lighting`, which
	// is a historical accident of where its volumetric renderer read the data
	// from; putting it with the rest of the sky is the arrangement that survives
	// somebody asking where the sky is configured.
	//
	// @since v0.19
	struct Clouds {
		// What the lit side of the cloud is.
		core::Color3 Colour{1.0f, 1.0f, 1.0f};

		// How much of the sky the layer covers, 0 to 1.
		float Cover = 0.5f;

		// How opaque the covered part is, 0 to 1.
		//
		// **Separate from `Cover`, because they are different weather.** Thin
		// total cover is an overcast day and thick partial cover is a scattering
		// of cumulus, and one number cannot say both.
		float Density = 0.7f;

		// How fast the layer drifts, in metres per second.
		float WindSpeed = 6.0f;

		// Which way it drifts across the horizontal X/Z plane.
		//
		// X addresses world X and Y addresses world Z. The renderer normalises the
		// pair, so this remains a direction and `WindSpeed` remains the magnitude.
		core::Vector2 WindDirection{1.0f, 0.0f};

		// Whether the layer is drawn at all.
		bool Enabled = true;

		// Explicit padding, for the reason `Components.hpp` opens with.
		uint8_t Reserved[3] = {};
	};

	// Six authored cube faces. The names resolve through the ordinary content
	// catalogue, so a sky uses the same CDN and resident texture table as a part.
	//
	// @since v0.19
	struct SkyboxTextures {
		core::Name Front;
		core::Name Back;
		core::Name Left;
		core::Name Right;
		core::Name Up;
		core::Name Down;
		bool Enabled = true;
		uint8_t Reserved[3] = {};
	};

	// Built-in device programs exposed as a closed property list. The authored
	// value saves by name, while the resolved enum is what the compute shader
	// consumes.
	//
	// @since v0.19
	enum class SkyboxComputeShader : uint8_t {
		Gradient,
		Sunset,
		Night,
		Nebula,
		Voxel,
	};

	constexpr size_t SKYBOX_COMPUTE_SHADER_COUNT = 5;

	// @since v0.19
	enum class CloudComputeShader : uint8_t {
		Cumulus,
		Stratus,
		Storm,
		Voxel,
	};

	constexpr size_t CLOUD_COMPUTE_SHADER_COUNT = 4;

	// @since v0.19
	enum class AtmosphereProceduralShader : uint8_t {
		Earth,
		Thin,
		Mars,
		Alien,
	};

	constexpr size_t ATMOSPHERE_PROCEDURAL_SHADER_COUNT = 4;

	const char *Describe(SkyboxComputeShader shader);
	const char *Describe(CloudComputeShader shader);
	const char *Describe(AtmosphereProceduralShader shader);
	SkyboxComputeShader SkyboxComputeShaderFromName(const core::Name &name);
	CloudComputeShader CloudComputeShaderFromName(const core::Name &name);
	AtmosphereProceduralShader AtmosphereProceduralShaderFromName(const core::Name &name);

	// A device-generated sky recipe. The generated image is derived render
	// state and never becomes another ECS copy of these values.
	//
	// @since v0.19
	struct SkyboxCompute {
		core::Color3 Zenith{0.08f, 0.24f, 0.55f};
		core::Color3 Horizon{0.62f, 0.76f, 0.92f};
		core::Color3 Ground{0.035f, 0.04f, 0.055f};
		float StarDensity = 0.0f;
		float SunSize = 0.025f;
		uint32_t Seed = 1;
		SkyboxComputeShader Shader = SkyboxComputeShader::Gradient;
		bool Enabled = true;
		uint8_t Reserved[2] = {};
	};

	// Extra volumetric controls carried by a `CloudCompute` instance. The base
	// colour, cover, density and wind remain the one `Clouds` component shared
	// with `CloudProcedural`.
	//
	// @since v0.19
	struct CloudCompute {
		float CellSize = 0.18f;
		float Detail = 0.55f;
		float Height = 0.28f;
		float Thickness = 0.12f;
		uint32_t Seed = 1;
		uint32_t Steps = 12;
		CloudComputeShader Shader = CloudComputeShader::Cumulus;
		bool Enabled = true;
		uint8_t Reserved[2] = {};
	};

	// Extra scattering controls carried by an `AtmosphereProcedural` instance.
	// The common authored appearance remains the `Atmosphere` component.
	//
	// @since v0.19
	struct AtmosphereProcedural {
		float PlanetRadius = 6371000.0f;
		float Height = 80000.0f;
		float Rayleigh = 1.0f;
		float Mie = 1.0f;
		uint32_t Samples = 12;
		AtmosphereProceduralShader Shader = AtmosphereProceduralShader::Earth;
		bool Enabled = true;
		uint8_t Reserved[2] = {};
	};

	enum class SkyboxSource : uint8_t {
		None,
		Textures,
		Compute,
	};

	// The one environment selected in deterministic hierarchy order.
	//
	// The booleans distinguish a missing instance from a present instance whose
	// authored density is zero. Render signatures use that distinction, while
	// simulation never consumes it.
	//
	// @since v0.19
	struct Environment {
		SkyboxTextures Textures;
		SkyboxCompute SkyCompute;
		Atmosphere Air;
		AtmosphereProcedural AirCompute;
		Clouds CloudLayer;
		CloudCompute CloudVolume;

		// Deterministic animation time supplied by the world's simulation clock.
		// It is resolved render state rather than authored component storage, so
		// moving clouds do not dirty or replicate their recipe sixty times a second.
		double CloudTime = 0.0;
		SkyboxSource Skybox = SkyboxSource::None;
		bool HasAtmosphere = false;
		bool HasClouds = false;
		// The selected atmosphere provider owns procedural controls.
		bool HasAtmosphereCompute = false;
		// The selected cloud provider owns compute controls.
		bool HasCloudCompute = false;
		uint8_t Reserved[3] = {};
	};

	// The world's atmosphere, or the defaults when it has none.
	//
	// **A free function rather than a lookup at every call site**, which is
	// `SunOf`'s arrangement: "no `Atmosphere` instance" and "an atmosphere of zero
	// density" have to be the same answer, so a world authored before this reads
	// as clear air rather than as a null a caller forgot to check.
	//
	// @param store The world.
	// @return Its atmosphere, clamped into range.
	Atmosphere AtmosphereOf(const ecs::Store &store);

	// The world's cloud layer, or the defaults when it has none.
	//
	// A world with no `Clouds` instance answers a layer with `Enabled` false,
	// which is the same "nothing to draw" a density of zero gives.
	//
	// @param store The world.
	// @return Its clouds, clamped into range.
	Clouds CloudsOf(const ecs::Store &store);

	// Resolves the first skybox, atmosphere and cloud provider beneath
	// `Lighting`. Each category is independent and follows tree order.
	//
	// @param store The world.
	// @return The selected authored environment.
	Environment EnvironmentOf(const ecs::Store &store);

	// The `Atmosphere` class id, registering the scene tree on first call.
	//
	// **Derives from `Instance` and not from `PVInstance`**, which is `Sound`'s
	// and `Attachment`'s omission for their reason: the air has no place in the
	// world, and a `Transform` on this row would be a second opinion about where
	// `Lighting` is.
	//
	// @return The class id.
	ecs::ClassId AtmosphereClass();
}
