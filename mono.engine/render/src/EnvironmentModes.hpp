#pragma once

// Device-free environment policy shared by uniform construction and its unit
// test. The numbers are the modes `environment.comp` consumes: zero disables a
// category, one uses its authored form and two uses its compute form.

#include <engine/scene/Atmosphere.hpp>

#include <cstdint>

namespace engine::render {

	struct EnvironmentUniformModes {
		uint32_t Skybox = 0;
		uint32_t Atmosphere = 0;
		uint32_t Clouds = 0;
	};

	struct EnvironmentUniformShaders {
		uint32_t Skybox = 0;
		uint32_t Atmosphere = 0;
		uint32_t Clouds = 0;
	};

	inline EnvironmentUniformModes EnvironmentModesOf(const scene::Environment &environment) {
		EnvironmentUniformModes modes;
		if (environment.Skybox == scene::SkyboxSource::Textures && environment.Textures.Enabled) {
			modes.Skybox = 1;
		} else if (environment.Skybox == scene::SkyboxSource::Compute && environment.SkyCompute.Enabled) {
			modes.Skybox = 2;
		}

		if (environment.HasAtmosphere) {
			modes.Atmosphere =
				environment.HasAtmosphereCompute ? (environment.AirCompute.Enabled ? 2u : 0u) : 1u;
		}
		if (environment.HasClouds && environment.CloudLayer.Enabled) {
			modes.Clouds = environment.HasCloudCompute ? (environment.CloudVolume.Enabled ? 2u : 0u) : 1u;
		}
		return modes;
	}

	inline EnvironmentUniformShaders EnvironmentShadersOf(const scene::Environment &environment) {
		const EnvironmentUniformModes modes = EnvironmentModesOf(environment);
		return EnvironmentUniformShaders{
			.Skybox = modes.Skybox == 2 ? static_cast<uint32_t>(environment.SkyCompute.Shader) : 0u,
			.Atmosphere = modes.Atmosphere == 2 ? static_cast<uint32_t>(environment.AirCompute.Shader) : 0u,
			.Clouds = modes.Clouds == 2 ? static_cast<uint32_t>(environment.CloudVolume.Shader) : 0u,
		};
	}

	inline scene::AtmosphereProcedural EnvironmentAtmosphereComputeOf(const scene::Environment &environment) {
		return EnvironmentModesOf(environment).Atmosphere == 2 ? environment.AirCompute
															   : scene::AtmosphereProcedural{};
	}

	inline scene::CloudCompute EnvironmentCloudComputeOf(const scene::Environment &environment) {
		return EnvironmentModesOf(environment).Clouds == 2 ? environment.CloudVolume : scene::CloudCompute{};
	}
}
