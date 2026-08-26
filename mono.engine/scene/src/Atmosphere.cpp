#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>

#include <algorithm>

namespace engine::scene {

	namespace {
		// The `Lighting` service, or a null entity.
		//
		// By class and never by name, which is this module's fixture rule: a
		// script renaming `Lighting` must not make the sky disappear.
		ecs::Entity LightingService(const ecs::Store &store) {
			return ServiceOf(store, ecs::Classes::Find(core::Name("Lighting")));
		}

		// Brings authored values into the range the header states.
		//
		// **Clamped on read rather than refused on write**, which is `LightingOf`'s
		// rule for `FogStart` and `FogEnd`: these arrive from a save file and a
		// wire as often as from a property setter, and a density above one is a
		// shader reading past the end of a lookup rather than a slightly thick
		// day.
		Atmosphere Clamped(Atmosphere air) {
			air.Density = std::clamp(air.Density, 0.0f, 1.0f);
			air.Offset = std::clamp(air.Offset, -1.0f, 1.0f);
			air.Glare = std::clamp(air.Glare, 0.0f, 10.0f);
			air.Haze = std::clamp(air.Haze, 0.0f, 10.0f);
			return air;
		}

		Clouds Clamped(Clouds sky) {
			sky.Cover = std::clamp(sky.Cover, 0.0f, 1.0f);
			sky.Density = std::clamp(sky.Density, 0.0f, 1.0f);
			sky.WindSpeed = std::max(sky.WindSpeed, 0.0f);
			if (sky.WindDirection.MagnitudeSquared() <= 0.0f) {
				sky.WindDirection = core::Vector2::XAxis;
			}
			return sky;
		}

		CloudCompute Clamped(CloudCompute cloud) {
			cloud.CellSize = std::max(cloud.CellSize, 0.001f);
			cloud.Detail = std::clamp(cloud.Detail, 0.0f, 1.0f);
			cloud.Height = std::clamp(cloud.Height, 0.0f, 1.0f);
			cloud.Thickness = std::clamp(cloud.Thickness, 0.001f, 1.0f);
			cloud.Steps = std::clamp(cloud.Steps, 1u, 64u);
			return cloud;
		}

		AtmosphereProcedural Clamped(AtmosphereProcedural air) {
			air.PlanetRadius = std::max(air.PlanetRadius, 1.0f);
			air.Height = std::max(air.Height, 1.0f);
			air.Rayleigh = std::max(air.Rayleigh, 0.0f);
			air.Mie = std::max(air.Mie, 0.0f);
			air.Samples = std::clamp(air.Samples, 1u, 64u);
			return air;
		}
	}

	const char *Describe(SkyboxComputeShader shader) {
		switch (shader) {
		case SkyboxComputeShader::Gradient:
			return "Gradient";
		case SkyboxComputeShader::Sunset:
			return "Sunset";
		case SkyboxComputeShader::Night:
			return "Night";
		case SkyboxComputeShader::Nebula:
			return "Nebula";
		case SkyboxComputeShader::Voxel:
			return "Voxel";
		}
		return "Gradient";
	}

	const char *Describe(CloudComputeShader shader) {
		switch (shader) {
		case CloudComputeShader::Cumulus:
			return "Cumulus";
		case CloudComputeShader::Stratus:
			return "Stratus";
		case CloudComputeShader::Storm:
			return "Storm";
		case CloudComputeShader::Voxel:
			return "Voxel";
		}
		return "Cumulus";
	}

	const char *Describe(AtmosphereProceduralShader shader) {
		switch (shader) {
		case AtmosphereProceduralShader::Earth:
			return "Earth";
		case AtmosphereProceduralShader::Thin:
			return "Thin";
		case AtmosphereProceduralShader::Mars:
			return "Mars";
		case AtmosphereProceduralShader::Alien:
			return "Alien";
		}
		return "Earth";
	}

	SkyboxComputeShader SkyboxComputeShaderFromName(const core::Name &name) {
		for (size_t index = 0; index < SKYBOX_COMPUTE_SHADER_COUNT; index++) {
			const auto shader = static_cast<SkyboxComputeShader>(index);
			if (name == core::Name(Describe(shader))) {
				return shader;
			}
		}
		return SkyboxComputeShader::Gradient;
	}

	CloudComputeShader CloudComputeShaderFromName(const core::Name &name) {
		for (size_t index = 0; index < CLOUD_COMPUTE_SHADER_COUNT; index++) {
			const auto shader = static_cast<CloudComputeShader>(index);
			if (name == core::Name(Describe(shader))) {
				return shader;
			}
		}
		return CloudComputeShader::Cumulus;
	}

	AtmosphereProceduralShader AtmosphereProceduralShaderFromName(const core::Name &name) {
		for (size_t index = 0; index < ATMOSPHERE_PROCEDURAL_SHADER_COUNT; index++) {
			const auto shader = static_cast<AtmosphereProceduralShader>(index);
			if (name == core::Name(Describe(shader))) {
				return shader;
			}
		}
		return AtmosphereProceduralShader::Earth;
	}

	Atmosphere AtmosphereOf(const ecs::Store &store) {
		return EnvironmentOf(store).Air;
	}

	Clouds CloudsOf(const ecs::Store &store) {
		return EnvironmentOf(store).CloudLayer;
	}

	Environment EnvironmentOf(const ecs::Store &store) {
		Environment environment;
		environment.CloudTime = store.Time().Elapsed;
		environment.Air.Density = 0.0f;
		environment.CloudLayer.Enabled = false;
		environment.AirCompute.Enabled = false;
		environment.CloudVolume.Enabled = false;

		const ecs::Entity lighting = LightingService(store);
		if (lighting == ecs::NULL_ENTITY) {
			return environment;
		}

		// One tree walk resolves all three independent categories. The first row
		// carrying a category wins even when disabled, so turning a provider off
		// cannot expose a lower sibling unexpectedly.
		store.EachDescendant(lighting, [&](ecs::Entity descendant) {
			if (environment.Skybox == SkyboxSource::None) {
				if (const SkyboxTextures *textures = store.Get<SkyboxTextures>(descendant)) {
					environment.Textures = *textures;
					environment.Skybox = SkyboxSource::Textures;
				} else if (const SkyboxCompute *compute = store.Get<SkyboxCompute>(descendant)) {
					environment.SkyCompute = *compute;
					environment.Skybox = SkyboxSource::Compute;
				}
			}

			if (!environment.HasAtmosphere) {
				if (const Atmosphere *air = store.Get<Atmosphere>(descendant)) {
					environment.Air = Clamped(*air);
					environment.HasAtmosphere = true;
					if (const AtmosphereProcedural *compute = store.Get<AtmosphereProcedural>(descendant)) {
						environment.HasAtmosphereCompute = true;
						environment.AirCompute = Clamped(*compute);
					}
				}
			}

			if (!environment.HasClouds) {
				if (const Clouds *clouds = store.Get<Clouds>(descendant)) {
					environment.CloudLayer = Clamped(*clouds);
					environment.HasClouds = true;
					if (const CloudCompute *compute = store.Get<CloudCompute>(descendant)) {
						environment.HasCloudCompute = true;
						environment.CloudVolume = Clamped(*compute);
					}
				}
			}
		});
		return environment;
	}

	ecs::ClassId AtmosphereClass() {
		// Through the one tree registration, for `AttachmentClass`'s reason.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Atmosphere"));
	}
}
