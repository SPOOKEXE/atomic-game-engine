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

	inline EnvironmentUniformModes EnvironmentModesOf(const scene::Environment &environment) {
		EnvironmentUniformModes modes;
		if (environment.Skybox == scene::SkyboxSource::Textures && environment.Textures.Enabled) {
			modes.Skybox = 1;
		} else if (environment.Skybox == scene::SkyboxSource::Compute && environment.SkyCompute.Enabled) {
			modes.Skybox = 2;
		}

		if (environment.HasAtmosphere) {
			modes.Atmosphere = environment.AirCompute.Enabled ? 2u : 1u;
		}
		if (environment.HasClouds && environment.CloudLayer.Enabled) {
			modes.Clouds = environment.CloudVolume.Enabled ? 2u : 1u;
		}
		return modes;
	}
}
