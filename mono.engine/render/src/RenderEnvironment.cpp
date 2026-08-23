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
		constexpr uint32_t ENVIRONMENT_WIDTH = 1024;
		constexpr uint32_t ENVIRONMENT_HEIGHT = 512;
		constexpr uint64_t ENVIRONMENT_KEEP_FRAMES = 240;

		bool Active(const scene::Environment &environment) {
			const bool textureSky =
				environment.Textures.Front.IsValid() || environment.Textures.Back.IsValid() ||
				environment.Textures.Left.IsValid() || environment.Textures.Right.IsValid() ||
				environment.Textures.Up.IsValid() || environment.Textures.Down.IsValid();
			const bool skybox =
				(environment.Skybox == scene::SkyboxSource::Textures && environment.Textures.Enabled &&
				 textureSky) ||
				(environment.Skybox == scene::SkyboxSource::Compute && environment.SkyCompute.Enabled);
			const bool atmosphere =
				environment.HasAtmosphere && (environment.Air.Density > 0.0f || environment.Air.Haze > 0.0f ||
											  environment.Air.Glare > 0.0f);
			const bool clouds = environment.HasClouds && environment.CloudLayer.Enabled &&
								environment.CloudLayer.Cover > 0.0f && environment.CloudLayer.Density > 0.0f;
			return skybox || atmosphere || clouds;
		}

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
			glm::uvec4 Modes;
			glm::uvec4 Counts;
			glm::uvec4 Shaders;
		};
	}

	SDL_GPUTexture *Renderer::Impl::EnsureEnvironment(
		uint64_t world,
		const scene::Environment &environment,
		SDL_GPUCommandBuffer *command,
		uint32_t &dispatches
	) {
		ENGINE_PROFILE_CAT("environment compute", core::ProfileCategory::Render);
		std::erase_if(Environments, [this](const EnvironmentTarget &candidate) {
			if (FrameCounter - candidate.LastUsedFrame <= ENVIRONMENT_KEEP_FRAMES) {
				return false;
			}
			if (candidate.Texture != nullptr) {
				gpu::ReleaseTexture(Device, candidate.Texture);
			}
			return true;
		});
		if (!Active(environment) || EnvironmentCompute == nullptr || command == nullptr) {
			return nullptr;
		}

		EnvironmentTarget *target = nullptr;
		for (EnvironmentTarget &candidate : Environments) {
			if (candidate.World == world) {
				target = &candidate;
				break;
			}
		}
		if (target == nullptr) {
			target = &Environments.emplace_back();
			target->World = world;
		}
		target->LastUsedFrame = FrameCounter;

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
		uint64_t signature = scene::MixSignature(1, static_cast<uint8_t>(environment.Skybox));
		for (size_t index = 0; index < faces.size(); index++) {
			faces[index] = Textures.Find(names[index]);
			if (names[index].IsValid() && faces[index] != nullptr) {
				faceMask |= 1u << index;
			}
			if (faces[index] == nullptr) {
				faces[index] = Textures.Missing();
			}
			signature = scene::MixSignature(signature, names[index].Id());
			signature = scene::MixSignature(
				signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(faces[index]))
			);
		}
		signature = Fold(signature, environment.SkyCompute);
		signature = Fold(signature, environment.Air);
		signature = Fold(signature, environment.AirCompute);
		signature = Fold(signature, environment.CloudLayer);
		signature = Fold(signature, environment.CloudVolume);
		signature = Fold(signature, Sun);
		signature = scene::MixSignature(signature, environment.HasAtmosphere ? 1u : 0u);
		signature = scene::MixSignature(signature, environment.HasClouds ? 1u : 0u);
		if (target->Texture != nullptr && target->Signature == signature) {
			return target->Texture;
		}

		if (target->Texture == nullptr) {
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
			info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
			info.width = ENVIRONMENT_WIDTH;
			info.height = ENVIRONMENT_HEIGHT;
			info.layer_count_or_depth = 1;
			info.num_levels = 1;
			info.sample_count = SDL_GPU_SAMPLECOUNT_1;
			target->Texture = gpu::CreateTexture(Device, &info);
			if (target->Texture == nullptr) {
				return nullptr;
			}
		}

		SDL_GPUStorageTextureReadWriteBinding destination{};
		destination.texture = target->Texture;
		// An authored edit can land while the previous environment version is
		// still sampled by an in-flight frame. Cycling preserves that reader and
		// hands this rare regeneration a writable backing image.
		destination.cycle = true;
		SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &destination, 1, nullptr, 0);
		if (pass == nullptr) {
			return nullptr;
		}
		SDL_BindGPUComputePipeline(pass, EnvironmentCompute);
		std::array<SDL_GPUTextureSamplerBinding, 6> bindings{};
		for (size_t index = 0; index < bindings.size(); index++) {
			bindings[index] = SDL_GPUTextureSamplerBinding{faces[index], Textures.Sampler()};
		}
		SDL_BindGPUComputeSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));

		const scene::SkyboxCompute &sky = environment.SkyCompute;
		const scene::Atmosphere &air = environment.Air;
		const scene::AtmosphereProcedural &airCompute = environment.AirCompute;
		const scene::Clouds &clouds = environment.CloudLayer;
		const scene::CloudCompute &cloudCompute = environment.CloudVolume;
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
			.Clouds = glm::vec4{clouds.Cover, clouds.Density, clouds.WindSpeed, clouds.WindDirection},
			.CloudCompute =
				glm::vec4{
					cloudCompute.CellSize, cloudCompute.Detail, cloudCompute.Height, cloudCompute.Thickness
				},
			.Modes =
				glm::uvec4{
					static_cast<uint32_t>(environment.Skybox),
					environment.HasAtmosphere ? (airCompute.Enabled ? 2u : 1u) : 0u,
					environment.HasClouds && clouds.Enabled ? (cloudCompute.Enabled ? 2u : 1u) : 0u,
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
				static_cast<uint32_t>(sky.Shader),
				static_cast<uint32_t>(airCompute.Shader),
				static_cast<uint32_t>(cloudCompute.Shader),
				0u,
			},
		};
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (ENVIRONMENT_WIDTH + 7) / 8, (ENVIRONMENT_HEIGHT + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		target->Signature = signature;
		dispatches++;
		return target->Texture;
	}

	void Renderer::Impl::ReleaseEnvironments() {
		for (EnvironmentTarget &environment : Environments) {
			if (environment.Texture != nullptr) {
				gpu::ReleaseTexture(Device, environment.Texture);
			}
		}
		Environments.clear();
	}
}
