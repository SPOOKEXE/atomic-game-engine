#include "EnvironmentModes.hpp"
#include "GpuHeap.hpp"
#include "RendererState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cmath>
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

		uint64_t BlackSkySignature(core::Name contentOwner) {
			uint64_t signature = scene::MixSignature(1, contentOwner.Id());
			signature = scene::MixSignature(signature, 0);
			return scene::MixSignature(signature, 0);
		}

		uint64_t BlackCloudSignature(core::Name contentOwner, SDL_GPUTexture *source, uint64_t skySignature) {
			uint64_t signature = scene::MixSignature(1, contentOwner.Id());
			signature =
				scene::MixSignature(signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(source)));
			signature = scene::MixSignature(signature, skySignature);
			signature = scene::MixSignature(signature, 0);
			return scene::MixSignature(signature, true);
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
		uint32_t &dispatches,
		const std::function<void()> &beginWork
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
		std::array<core::Name, 6> faceOwners{};
		uint32_t faceMask = 0;
		const EnvironmentUniformModes modes = EnvironmentModesOf(environment);
		for (size_t index = 0; index < faces.size(); index++) {
			if (modes.Skybox == 1) {
				faceOwners[index] = TextureContentOwnerWithFallback(
					names[index], ActiveContentOwner, ActiveLocalImageGraphWorld, ActiveImageGraphWorld
				);
				faces[index] = Textures.Find(names[index], faceOwners[index]);
				if (names[index].IsValid() && faces[index] != nullptr) {
					faceMask |= 1u << index;
				}
			}
			faces[index] = faces[index] == nullptr ? Textures.Missing() : faces[index];
		}
		uint64_t signature = BlackSkySignature(ActiveContentOwner);
		if (modes.Skybox != 0 || modes.Atmosphere != 0) {
			signature = scene::MixSignature(1, ActiveContentOwner.Id());
			signature = scene::MixSignature(signature, modes.Skybox);
			signature = scene::MixSignature(signature, modes.Atmosphere);
		}
		if (modes.Skybox == 1) {
			for (size_t index = 0; index < faces.size(); index++) {
				signature = scene::MixSignature(signature, names[index].Id());
				signature =
					scene::MixSignature(signature, Textures.RevisionOf(names[index], faceOwners[index]));
				signature = scene::MixSignature(
					signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(faces[index]))
				);
			}
		}
		// Texture skies use the procedural colours for missing faces. Disabled
		// providers do not consume their authored values in environment.comp.
		if (modes.Skybox != 0) {
			signature = Fold(signature, environment.SkyCompute);
		}
		if (modes.Atmosphere != 0) {
			signature = Fold(signature, environment.Air);
		}
		if (modes.Atmosphere == 2) {
			signature = Fold(signature, EnvironmentAtmosphereComputeOf(environment));
		}
		if (modes.Skybox != 0 || modes.Atmosphere != 0) {
			signature = Fold(signature, Sun);
		}
		if (cache->Sky.Matches(signature, command)) return true;
		beginWork();
		if (modes.Skybox == 0 && modes.Atmosphere == 0) {
			SDL_GPUColorTargetInfo clear{};
			clear.texture = destinationTexture;
			clear.clear_color = {0, 0, 0, 1};
			clear.load_op = SDL_GPU_LOADOP_CLEAR;
			clear.store_op = SDL_GPU_STOREOP_STORE;
			clear.cycle = true;
			auto *pass = SDL_BeginGPURenderPass(command, &clear, 1, nullptr);
			if (pass == nullptr) return false;
			SDL_EndGPURenderPass(pass);
			cache->Sky.Stage(signature, command);
			core::Metrics::Count("render.empty_environment.clears", 1);
			core::Metrics::Count("render.colour_clear.commands", 1);
			core::Metrics::Count(
				"render.colour_clear.bytes",
				SDL_CalculateGPUTextureFormatSize(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, width, height, 1)
			);
			return true;
		}
		SDL_GPUStorageTextureReadWriteBinding destination{};
		destination.texture = destinationTexture;
		// The history target may still be sampled by an earlier submitted command
		// buffer. Cycling gives this regeneration separate backing storage.
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
		cache->Sky.Stage(signature, command);
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
		uint32_t &dispatches,
		bool opaqueSkySource,
		const std::function<void()> &beginWork,
		bool weatherIntegrationEnabled
	) {
		ENGINE_PROFILE_CAT("cloud environment compute", core::ProfileCategory::Render);
		if (EnvironmentCloudCompute == nullptr || command == nullptr || source == nullptr ||
			destinationTexture == nullptr || width == 0 || height == 0) {
			ENGINE_ERROR(
				"cloud environment compute unavailable: pipeline={}, command={}, source={}, destination={}, "
				"extent={}x{}",
				EnvironmentCloudCompute != nullptr,
				command != nullptr,
				source != nullptr,
				destinationTexture != nullptr,
				width,
				height
			);
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
		scene::CloudCompute cloudCompute =
			environment.HasWeatherCompute ? environment.CloudVolume : EnvironmentCloudComputeOf(environment);
		EnvironmentUniformModes modes = EnvironmentModesOf(environment);
		// World weather is integrated after geometry instead of painted onto the sky cube.
		if (modes.Clouds == 3) {
			// Suppression follows the graph that will actually consume the volume.
			// A later product refusal is handled by the view's fragment fallback.
			modes.Clouds = weatherIntegrationEnabled ? 0u : 2u;
			cloudCompute.Seed = environment.Weather.Seed;
		}
		auto shaders = EnvironmentShadersOf(environment);
		if (environment.HasWeatherCompute && modes.Clouds != 0)
			shaders.Clouds = static_cast<uint32_t>(cloudCompute.Shader);
		uint64_t skySignature = 0;
		bool linkedSky = false;
		for (const EnvironmentTarget &candidate : Environments) {
			if (candidate.SkyTarget == source) {
				skySignature = candidate.Sky.SignatureFor(command);
				linkedSky = true;
				break;
			}
		}
		uint64_t signature;
		if (modes.Clouds == 0 && opaqueSkySource && linkedSky) {
			signature = BlackCloudSignature(ActiveContentOwner, source, skySignature);
		} else {
			signature = scene::MixSignature(1, ActiveContentOwner.Id());
			signature =
				scene::MixSignature(signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(source)));
			if (linkedSky) signature = scene::MixSignature(signature, skySignature);
			signature = scene::MixSignature(signature, modes.Clouds);
			signature = scene::MixSignature(signature, opaqueSkySource);
		}
		if (modes.Clouds != 0) {
			signature = Fold(signature, environment.CloudLayer);
			signature = Fold(signature, cloudCompute);
			if (environment.HasWeatherCompute) {
				signature = Fold(signature, environment.Weather.Coverage);
				signature = Fold(signature, environment.Weather.WindMetresPerSecond);
				signature = Fold(signature, environment.Clock.ElapsedSeconds);
				signature = Fold(signature, environment.Clock.ElapsedFractionQ32);
				signature = Fold(signature, Sun);
				signature = Fold(signature, Ambient);
				signature = Fold(signature, OutdoorAmbient);
				signature = Fold(signature, Direct);
			}
			if (environment.CloudLayer.WindSpeed > 0.0f) {
				signature = Fold(signature, environment.CloudTime);
			}
		}
		// Arbitrary authored inputs can change in place and can contain signed RGB or alpha.
		if (opaqueSkySource && cache->Cloud.Matches(signature, command)) return true;
		beginWork();
		if (modes.Clouds == 0 && opaqueSkySource && source != destinationTexture) {
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (copy == nullptr) return false;
			SDL_GPUTextureLocation from{}, to{};
			from.texture = source;
			to.texture = destinationTexture;
			SDL_CopyGPUTextureToTexture(copy, &from, &to, width, height, 1, true);
			SDL_EndGPUCopyPass(copy);
			cache->Cloud.Stage(signature, command);
			core::Metrics::Count("render.identity_environment_copy.commands", 1);
			core::Metrics::Count(
				"render.identity_environment_copy.bytes",
				SDL_CalculateGPUTextureFormatSize(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, width, height, 1)
			);
			return true;
		}
		auto cloudColour = Colour(clouds.Colour);
		glm::vec4 cloudCoverage{clouds.Cover, clouds.Density, clouds.WindDirection.X, clouds.WindDirection.Y};
		glm::vec4 motion{clouds.WindSpeed, static_cast<float>(environment.CloudTime), 0, 0};
		if (environment.HasWeatherCompute && modes.Clouds != 0) {
			const float sunLength = std::sqrt(Sun.x * Sun.x + Sun.y * Sun.y + Sun.z * Sun.z);
			const float sunlight =
				sunLength > 0 && std::isfinite(sunLength) ? std::max(0.0f, -Sun.y / sunLength) : 1;
			cloudColour *= Ambient + OutdoorAmbient * .5f + Direct * sunlight * .35f;
			cloudCoverage = {
				environment.Weather.Coverage,
				clouds.Density * std::clamp(clouds.Cover, 0.0f, 1.0f),
				environment.Weather.WindMetresPerSecond.X,
				environment.Weather.WindMetresPerSecond.Z
			};
			motion.x = std::hypot(
				environment.Weather.WindMetresPerSecond.X, environment.Weather.WindMetresPerSecond.Z
			);
			motion.y = float(environment.Clock.ElapsedSeconds % 4096) +
					   float(double(environment.Clock.ElapsedFractionQ32) / 4294967296.0);
		}
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
			.CloudColour = cloudColour,
			.Clouds = cloudCoverage,
			.CloudCompute =
				glm::vec4{
					cloudCompute.CellSize, cloudCompute.Detail, cloudCompute.Height, cloudCompute.Thickness
				},
			.CloudMotion = motion,
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
		// A previous frame can still sample this history target when this command
		// buffer begins, so this write needs fresh backing storage.
		destination.cycle = true;
		SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &destination, 1, nullptr, 0);
		if (pass == nullptr) {
			ENGINE_ERROR("cloud environment compute pass: {}", SDL_GetError());
			return false;
		}
		SDL_BindGPUComputePipeline(pass, EnvironmentCloudCompute);
		const SDL_GPUTextureSamplerBinding binding{source, Textures.Sampler()};
		SDL_BindGPUComputeSamplers(pass, 0, &binding, 1);
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		cache->Cloud.Stage(signature, command);
		dispatches++;
		return true;
	}

	bool Renderer::Impl::IsBlackEnvironment(
		const scene::Environment &environment, const void *command, SDL_GPUTexture *texture
	) const {
		if (texture == nullptr) return false;
		const EnvironmentUniformModes modes = EnvironmentModesOf(environment);
		if (modes.Skybox != 0 || modes.Atmosphere != 0 || modes.Clouds != 0) return false;

		const uint64_t skySignature = BlackSkySignature(ActiveContentOwner);
		for (const EnvironmentTarget &sky : Environments) {
			if (sky.SkyTarget == nullptr || sky.Sky.SignatureFor(command) != skySignature) continue;
			if (sky.SkyTarget == texture) return true;
			const uint64_t cloudSignature =
				BlackCloudSignature(ActiveContentOwner, sky.SkyTarget, skySignature);
			for (const EnvironmentTarget &cloud : Environments) {
				if (cloud.CloudTarget == texture && cloud.Cloud.SignatureFor(command) == cloudSignature)
					return true;
			}
		}
		return false;
	}

	void Renderer::Impl::ReleaseEnvironments() {
		// Graph targets own the images. This cache only owns their dirty signatures.
		Environments.clear();
		ReleaseWeatherTemporalHistories();
		for (auto &owner : WeatherPhysicalOwners)
			ReleaseWeatherSimulationGpu(Device, owner->Physical);
		WeatherPhysicalOwners.clear();
		WeatherGenerations.clear();
	}

	void Renderer::Impl::ForgetWeatherWorld(uint64_t world) {
		ForgetWeatherTemporalWorld(world);
		std::erase_if(WeatherGenerations, [world](const WeatherGpuGeneration &generation) {
			return generation.World == world;
		});
		PruneWeatherPhysicalOwners();
		// SDL defers the actual destruction until queued references retire. Disabled
		// layers release payloads immediately rather than retaining a world-sized cache.
		std::erase_if(GraphResources.Buffers, [this, world](const GraphBuffer &buffer) {
			const bool weatherBuffer = buffer.Resource == core::Name("weather-pages") ||
									   buffer.Resource == core::Name("weather-previous-pages") ||
									   buffer.Resource == core::Name("weather-field") ||
									   buffer.Resource == core::Name("weather-summary");
			if (buffer.Owner != world || buffer.Scope != graph::NodeScope::World || !weatherBuffer)
				return false;
			if (buffer.Buffer) gpu::ReleaseBuffer(Device, buffer.Buffer);
			return true;
		});
		std::erase_if(GraphResources.Targets, [this, world](const GraphTarget &target) {
			if (target.Owner != world || target.Scope != graph::NodeScope::World ||
				target.Resource != core::Name("weather-shadow"))
				return false;
			if (target.Texture) gpu::ReleaseTexture(Device, target.Texture);
			return true;
		});
	}

	void Renderer::Impl::ReleaseWeatherPipelines() {
		for (auto **pipeline :
			 {&WeatherFieldCompute,
			  &WeatherSummaryCompute,
			  &WeatherRaysCompute,
			  &WeatherEvolutionCompute,
			  &WeatherTemporalCompute,
			  &WeatherReconstructCompute,
			  &WeatherShadowCompute}) {
			if (*pipeline) SDL_ReleaseGPUComputePipeline(Device, *pipeline);
			*pipeline = nullptr;
		}
		for (auto &owner : WeatherPhysicalOwners)
			ReleaseWeatherSimulationGpu(Device, owner->Physical);
		WeatherPhysicalOwners.clear();
		WeatherGenerations.clear();
		ReleaseWeatherTemporalHistories();
	}

	void Renderer::Impl::CommitEnvironmentWrites(SDL_GPUCommandBuffer *command) {
		CommitWeatherTemporalWrites(command);
		for (WeatherGpuGeneration &generation : WeatherGenerations) {
			generation.Shadows.Commit(command);
			generation.Residency.Commit(command);
			if (generation.Field.Pending && generation.Field.PendingCommand == command &&
				generation.PendingRenderInputs) {
				generation.RenderInputs = std::move(generation.PendingRenderInputs);
				generation.PendingRenderInputs.reset();
			}
			generation.Field.Commit(command);
			generation.Summary.Commit(command);
		}
		for (EnvironmentTarget &target : Environments) {
			target.Sky.Commit(command);
			target.Cloud.Commit(command);
		}
		PruneWeatherPhysicalOwners();
	}

	WeatherPhysicalOwner &Renderer::Impl::WeatherPhysicalFor(uint64_t world, uint64_t recipeSignature) {
		for (auto &owner : WeatherPhysicalOwners)
			if (owner->World == world && owner->RecipeSignature == recipeSignature) return *owner;
		auto owner = std::make_unique<WeatherPhysicalOwner>();
		owner->World = world;
		owner->RecipeSignature = recipeSignature;
		WeatherPhysicalOwners.push_back(std::move(owner));
		core::Metrics::SetGauge("render.weather.stateful.physical_owners", WeatherPhysicalOwners.size());
		return *WeatherPhysicalOwners.back();
	}

	void Renderer::Impl::PruneWeatherPhysicalOwners() {
		std::erase_if(WeatherPhysicalOwners, [this](const auto &owner) {
			const bool referenced = std::any_of(
				WeatherGenerations.begin(),
				WeatherGenerations.end(),
				[&](const WeatherGpuGeneration &generation) {
					return generation.PhysicalOwner == owner.get();
				}
			);
			if (referenced) return false;
			ReleaseWeatherSimulationGpu(Device, owner->Physical);
			return true;
		});
		core::Metrics::SetGauge("render.weather.stateful.physical_owners", WeatherPhysicalOwners.size());
	}

	void Renderer::Impl::DiscardEnvironmentWrites(SDL_GPUCommandBuffer *command) {
		DiscardWeatherTemporalWrites(command);
		for (WeatherGpuGeneration &generation : WeatherGenerations) {
			generation.Shadows.Discard(command);
			for (auto *content : {&generation.Residency, &generation.Field, &generation.Summary}) {
				if (content->Pending && (!command || content->PendingCommand == command)) {
					// Cycling may have selected unwritten backing before cancellation.
					content->Ready = false;
					content->Discard(command);
					if (content == &generation.Field) generation.PendingRenderInputs.reset();
				}
			}
		}
		for (EnvironmentTarget &target : Environments) {
			target.Sky.Discard(command);
			target.Cloud.Discard(command);
		}
		PruneWeatherPhysicalOwners();
	}
}
