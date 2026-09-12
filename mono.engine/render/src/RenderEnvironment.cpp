#include "EnvironmentModes.hpp"
#include "GpuHeap.hpp"
#include "RendererState.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <type_traits>

namespace engine::render {
	namespace {
		template <class Value> uint64_t Fold(uint64_t signature, const Value &value) {
			static_assert(std::is_trivially_copyable_v<Value>);
			for (const std::byte byte : std::as_bytes(std::span<const Value>(&value, 1))) {
				signature = scene::MixSignature(signature, std::to_integer<uint8_t>(byte));
			}
			return signature;
		}

		glm::vec4 Colour(const core::Color3 &colour, float alpha = 1.0f) {
			return glm::vec4{colour.R, colour.G, colour.B, alpha};
		}

		struct EnvironmentUniforms {
			glm::vec4 Zenith;
			glm::vec4 Horizon;
			glm::vec4 Ground;
			glm::vec4 SunDirection;
			glm::vec4 AtmosphereColour;
			glm::vec4 AtmosphereDecay;
			glm::vec4 Atmosphere;
			glm::vec4 AtmosphereCompute;
			glm::vec4 CloudColour;
			glm::vec4 Clouds;
			glm::vec4 CloudCompute;
			glm::vec4 CloudMotion;
			glm::uvec4 Modes;
			glm::uvec4 Counts;
			glm::uvec4 Shaders;
		};
	}

	bool Renderer::Impl::RecordEnvironmentSkybox(
		const scene::Environment &environment,
		SDL_GPUCommandBuffer *command,
		SDL_GPUTexture *destinationTexture,
		uint32_t width,
		uint32_t height,
		uint32_t &dispatches
	) {
		ENGINE_PROFILE_CAT("environment compute", core::ProfileCategory::Render);
		if (EnvironmentSkyCompute == nullptr || command == nullptr || destinationTexture == nullptr ||
			width == 0 || height == 0) {
			return false;
		}
		EnvironmentTarget *cache = nullptr;
		for (EnvironmentTarget &candidate : Environments) {
			if (candidate.SkyTarget == destinationTexture) {
				cache = &candidate;
				break;
			}
		}
		if (cache == nullptr) {
			cache = &Environments.emplace_back();
			cache->SkyTarget = destinationTexture;
		}

		const std::array names{
			environment.Textures.Front,
			environment.Textures.Back,
			environment.Textures.Left,
			environment.Textures.Right,
			environment.Textures.Up,
			environment.Textures.Down,
		};
		std::array<SDL_GPUTexture *, 6> faces{};
		uint32_t faceMask = 0;
		const EnvironmentUniformModes modes = EnvironmentModesOf(environment);
		for (size_t index = 0; index < faces.size(); index++) {
			if (modes.Skybox == 1) {
				faces[index] =
					Textures.Find(names[index], TextureContentOwner(names[index], ActiveContentOwner));
				if (names[index].IsValid() && faces[index] != nullptr) {
					faceMask |= 1u << index;
				}
			}
			faces[index] = faces[index] == nullptr ? Textures.Missing() : faces[index];
		}
		uint64_t signature = scene::MixSignature(1, ActiveContentOwner.Id());
		signature = scene::MixSignature(signature, modes.Skybox);
		signature = scene::MixSignature(signature, modes.Atmosphere);
		for (size_t index = 0; index < faces.size(); index++) {
			signature = scene::MixSignature(signature, names[index].Id());
			signature = scene::MixSignature(
				signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(faces[index]))
			);
		}
		signature = Fold(signature, environment.SkyCompute);
		signature = Fold(signature, environment.Air);
		signature = Fold(signature, EnvironmentAtmosphereComputeOf(environment));
		signature = Fold(signature, Sun);
		if (cache->SkyReady && cache->SkySignature == signature) return true;
		SDL_GPUStorageTextureReadWriteBinding destination{};
		destination.texture = destinationTexture;
		// An authored edit can land while the previous environment version is
		// still sampled by an in-flight frame. Cycling preserves that reader and
		// hands this rare regeneration a writable backing image.
		destination.cycle = true;
		SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &destination, 1, nullptr, 0);
		if (pass == nullptr) {
			return false;
		}
		SDL_BindGPUComputePipeline(pass, EnvironmentSkyCompute);
		std::array<SDL_GPUTextureSamplerBinding, 6> bindings{};
		for (size_t index = 0; index < bindings.size(); index++) {
			bindings[index] = SDL_GPUTextureSamplerBinding{faces[index], Textures.Sampler()};
		}
		SDL_BindGPUComputeSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));

		const scene::SkyboxCompute &sky = environment.SkyCompute;
		const scene::Atmosphere &air = environment.Air;
		const scene::AtmosphereProcedural airCompute = EnvironmentAtmosphereComputeOf(environment);
		const scene::Clouds &clouds = environment.CloudLayer;
		const scene::CloudCompute cloudCompute = EnvironmentCloudComputeOf(environment);
		const EnvironmentUniformShaders shaders = EnvironmentShadersOf(environment);
		const EnvironmentUniforms uniforms{
			.Zenith = Colour(sky.Zenith, sky.StarDensity),
			.Horizon = Colour(sky.Horizon, sky.SunSize),
			.Ground = Colour(sky.Ground, 1.0f),
			.SunDirection = glm::vec4{Sun, 0.0f},
			.AtmosphereColour = Colour(air.Colour),
			.AtmosphereDecay = Colour(air.Decay),
			.Atmosphere = glm::vec4{air.Density, air.Offset, air.Glare, air.Haze},
			.AtmosphereCompute =
				glm::vec4{airCompute.PlanetRadius, airCompute.Height, airCompute.Rayleigh, airCompute.Mie},
			.CloudColour = Colour(clouds.Colour),
			.Clouds =
				glm::vec4{
					clouds.Cover,
					clouds.Density,
					clouds.WindDirection.X,
					clouds.WindDirection.Y,
				},
			.CloudCompute =
				glm::vec4{
					cloudCompute.CellSize, cloudCompute.Detail, cloudCompute.Height, cloudCompute.Thickness
				},
			.CloudMotion = glm::vec4{clouds.WindSpeed, static_cast<float>(environment.CloudTime), 0.0f, 0.0f},
			.Modes =
				glm::uvec4{
					modes.Skybox,
					modes.Atmosphere,
					modes.Clouds,
					faceMask,
				},
			.Counts =
				glm::uvec4{
					sky.Seed,
					cloudCompute.Seed,
					std::clamp(airCompute.Samples, 1u, 64u),
					std::clamp(cloudCompute.Steps, 1u, 64u),
				},
			.Shaders = glm::uvec4{
				shaders.Skybox,
				shaders.Atmosphere,
				shaders.Clouds,
				0u,
			},
		};
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		cache->SkySignature = signature;
		cache->SkyReady = true;
		dispatches++;
		return true;
	}

	bool Renderer::Impl::RecordEnvironmentClouds(
		const scene::Environment &environment,
		SDL_GPUCommandBuffer *command,
		SDL_GPUTexture *source,
		SDL_GPUTexture *destinationTexture,
		uint32_t width,
		uint32_t height,
		uint32_t &dispatches
	) {
		ENGINE_PROFILE_CAT("cloud environment compute", core::ProfileCategory::Render);
		if (EnvironmentCloudCompute == nullptr || command == nullptr || source == nullptr ||
			destinationTexture == nullptr || width == 0 || height == 0) {
			return false;
		}
		EnvironmentTarget *cache = nullptr;
		for (EnvironmentTarget &candidate : Environments) {
			if (candidate.CloudTarget == destinationTexture) {
				cache = &candidate;
				break;
			}
		}
		if (cache == nullptr) {
			cache = &Environments.emplace_back();
			cache->CloudTarget = destinationTexture;
		}

		const scene::SkyboxCompute &sky = environment.SkyCompute;
		const scene::Atmosphere &air = environment.Air;
		const scene::AtmosphereProcedural airCompute = EnvironmentAtmosphereComputeOf(environment);
		const scene::Clouds &clouds = environment.CloudLayer;
		const scene::CloudCompute cloudCompute = EnvironmentCloudComputeOf(environment);
		const EnvironmentUniformModes modes = EnvironmentModesOf(environment);
		const EnvironmentUniformShaders shaders = EnvironmentShadersOf(environment);
		uint64_t signature = scene::MixSignature(1, ActiveContentOwner.Id());
		signature =
			scene::MixSignature(signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(source)));
		for (const EnvironmentTarget &candidate : Environments) {
			if (candidate.SkyTarget == source) {
				signature = scene::MixSignature(signature, candidate.SkySignature);
				break;
			}
		}
		signature = scene::MixSignature(signature, modes.Clouds);
		signature = Fold(signature, environment.CloudLayer);
		signature = Fold(signature, EnvironmentCloudComputeOf(environment));
		if (environment.CloudLayer.WindSpeed > 0.0f) {
			signature = Fold(signature, environment.CloudTime);
		}
		if (cache->CloudReady && cache->CloudSignature == signature) return true;
		const EnvironmentUniforms uniforms{
			.Zenith = Colour(sky.Zenith, sky.StarDensity),
			.Horizon = Colour(sky.Horizon, sky.SunSize),
			.Ground = Colour(sky.Ground, 1.0f),
			.SunDirection = glm::vec4{Sun, 0.0f},
			.AtmosphereColour = Colour(air.Colour),
			.AtmosphereDecay = Colour(air.Decay),
			.Atmosphere = glm::vec4{air.Density, air.Offset, air.Glare, air.Haze},
			.AtmosphereCompute =
				glm::vec4{airCompute.PlanetRadius, airCompute.Height, airCompute.Rayleigh, airCompute.Mie},
			.CloudColour = Colour(clouds.Colour),
			.Clouds = glm::vec4{clouds.Cover, clouds.Density, clouds.WindDirection.X, clouds.WindDirection.Y},
			.CloudCompute =
				glm::vec4{
					cloudCompute.CellSize, cloudCompute.Detail, cloudCompute.Height, cloudCompute.Thickness
				},
			.CloudMotion = glm::vec4{clouds.WindSpeed, static_cast<float>(environment.CloudTime), 0.0f, 0.0f},
			.Modes = glm::uvec4{modes.Skybox, modes.Atmosphere, modes.Clouds, 0u},
			.Counts =
				glm::uvec4{
					sky.Seed,
					cloudCompute.Seed,
					std::clamp(airCompute.Samples, 1u, 64u),
					std::clamp(cloudCompute.Steps, 1u, 64u),
				},
			.Shaders = glm::uvec4{shaders.Skybox, shaders.Atmosphere, shaders.Clouds, 0u},
		};

		SDL_GPUStorageTextureReadWriteBinding destination{};
		destination.texture = destinationTexture;
		destination.cycle = true;
		SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &destination, 1, nullptr, 0);
		if (pass == nullptr) return false;
		SDL_BindGPUComputePipeline(pass, EnvironmentCloudCompute);
		const SDL_GPUTextureSamplerBinding binding{source, Textures.Sampler()};
		SDL_BindGPUComputeSamplers(pass, 0, &binding, 1);
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		cache->CloudSignature = signature;
		cache->CloudReady = true;
		dispatches++;
		return true;
	}

	void Renderer::Impl::ReleaseEnvironments() {
		// Graph targets own the images. This cache only owns their dirty signatures.
		Environments.clear();
	}
}
