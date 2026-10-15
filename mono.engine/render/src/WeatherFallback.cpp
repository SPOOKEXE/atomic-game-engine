#include "WeatherFallback.hpp"

#include "EnvironmentModes.hpp"
#include "ViewRecording.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/weather/Clock.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::render {
	bool WeatherVolumeGraphAvailable(
		const graph::RenderGraph &graph, bool computeAvailable, bool programsAvailable
	) {
		if (!computeAvailable || !programsAvailable) return false;
		const std::array kinds{
			core::Name("weather-residency"),
			core::Name("weather-field"),
			core::Name("weather-summary"),
			core::Name("weather-integrate"),
			core::Name("weather-reconstruct")
		};
		std::array<bool, kinds.size()> available{};
		for (uint32_t value = 1; value <= graph.Count(); ++value) {
			const auto *node = graph.Find(graph::NodeId{value});
			if (!node || !node->Enabled) continue;
			for (size_t index = 0; index < kinds.size(); ++index)
				available[index] = available[index] || node->Kind == kinds[index];
		}
		return std::all_of(available.begin(), available.end(), [](bool enabled) { return enabled; });
	}

	bool VisibleWeatherMedium(const scene::Environment &environment) {
		return environment.HasWeatherCompute && environment.HasClouds && environment.Weather.Enabled &&
			   environment.CloudLayer.Enabled && environment.CloudLayer.Cover > 0 &&
			   environment.CloudLayer.Density > 0;
	}

	bool WeatherProductsReady(const WeatherGpuGeneration *generation, const void *command) {
		if (!generation || !generation->Cells || !generation->Pages || !generation->PreviousPages ||
			!generation->Summaries)
			return false;
		const auto *inputs = generation->InputsFor(command);
		const uint64_t field = generation->Field.SignatureFor(command);
		return inputs && (!inputs->Stateful || inputs->Uniforms.Phase.z != UINT32_MAX) && field != 0 &&
			   generation->Residency.SignatureFor(command) != 0 &&
			   generation->Summary.SignatureFor(command) == field;
	}

	void RefuseWeatherPublication(WeatherGpuGeneration &generation) {
		generation.Residency = {};
		generation.Field = {};
		generation.Summary = {};
		generation.RenderInputs.reset();
		generation.PendingRenderInputs.reset();
	}

	bool ViewRecording::WeatherVolumeAvailable() const {
		const bool programs = State->WeatherFieldCompute && State->WeatherSummaryCompute &&
							  State->WeatherRaysCompute && State->WeatherTemporalCompute &&
							  State->WeatherReconstructCompute;
		const auto &environment = CurrentLighting.EnvironmentState;
		return WeatherVolumeGraphAvailable(Pipeline->Graph, State->Caps.HasCompute, programs) &&
			   scene::ValidWeatherCompute(environment.Weather) &&
			   (environment.Weather.Mode != scene::WeatherMode::Stateful || State->WeatherEvolutionCompute);
	}

	WeatherFallbackUniforms WeatherFallbackOf(
		const scene::WorldLighting &lighting,
		bool environmentTexture,
		bool drawSky,
		bool drawClouds,
		bool linearDepth,
		uint32_t faceMask
	) {
		const auto &environment = lighting.EnvironmentState;
		const auto &sky = environment.SkyCompute;
		const auto &air = environment.Air;
		const auto &appearance = environment.CloudLayer;
		const auto controls = scene::ClampedWeatherCompute(environment.Weather);
		const auto modes = EnvironmentModesOf(environment);
		const auto light = WeatherLightingOf(lighting);
		const auto colour = [](core::Color3 value, float alpha = 1) {
			return glm::vec4{value.R, value.G, value.B, alpha};
		};
		glm::vec2 displacement{};
		if (environment.HasWeatherClock) {
			if (const auto time = weather::WeatherTimeOf(environment.Clock)) {
				// A short seamless noise period keeps this cosmetic approximation precise
				// at huge clock ages. It is never fed back into the canonical field.
				const uint32_t period = controls.FeatureCells * 256u * 8u;
				displacement.x = float(time->DisplacementQ8[0] % period) * controls.CellSizeMetres / 256;
				displacement.y = float(time->DisplacementQ8[2] % period) * controls.CellSizeMetres / 256;
			}
		}
		const uint32_t flags = (environmentTexture ? 1u : 0u) | (drawSky ? 2u : 0u) | (drawClouds ? 4u : 0u) |
							   (linearDepth ? 8u : 0u) | (modes.Skybox == 1 ? 16u : 0u) |
							   (modes.Atmosphere != 0 ? 32u : 0u) | (modes.Skybox != 0 ? 64u : 0u);
		return {
			colour(sky.Zenith, sky.StarDensity),
			colour(sky.Horizon, sky.SunSize),
			colour(sky.Ground),
			colour(air.Colour),
			colour(air.Decay),
			{air.Density, air.Offset, air.Glare, air.Haze},
			{-controls.ExtentMetres.X * .5f, controls.BaseHeightMetres, -controls.ExtentMetres.Z * .5f, 0},
			{controls.ExtentMetres.X * .5f,
			 controls.BaseHeightMetres + controls.ThicknessMetres,
			 controls.ExtentMetres.Z * .5f,
			 0},
			colour(
				appearance.Colour,
				weather::ExtinctionPerMetre(controls.CondensateScale) * std::max(0.0f, appearance.Density) *
					std::clamp(appearance.Cover, 0.0f, 1.0f)
			),
			{controls.Coverage,
			 controls.CellSizeMetres * controls.FeatureCells,
			 displacement.x,
			 displacement.y},
			light.Direction,
			light.Direct,
			light.Ambient,
			{flags,
			 faceMask,
			 controls.Seed ^ weather::WeatherLayerKey(controls.LayerName.Text()),
			 static_cast<uint32_t>(sky.Shader) |
				 (static_cast<uint32_t>(environment.CloudVolume.Shader) << 8) |
				 (static_cast<uint32_t>(environment.AirCompute.Shader) << 16)}
		};
	}

	bool ViewRecording::RecordWeatherFallback(
		const graph::RunContext &context,
		const Impl::NamedTexture &source,
		const Impl::NamedTexture &depth,
		const Impl::NamedTexture &target,
		const Impl::NamedTexture &environment,
		bool drawSky,
		bool drawClouds,
		bool linearDepth,
		const PbrUniforms *captureUniforms,
		const scene::WorldLighting *captureLighting
	) {
		if (!State->WeatherFallbackPipeline || !source.IsValid() || !depth.IsValid() || !target.IsValid() ||
			source.Texture == target.Texture)
			return false;
		const auto &lighting = captureLighting ? *captureLighting : CurrentLighting;
		const auto &textures = lighting.EnvironmentState.Textures;
		const std::array names{
			textures.Front, textures.Back, textures.Left, textures.Right, textures.Up, textures.Down
		};
		std::array<SDL_GPUTextureSamplerBinding, 9> bindings{};
		bindings[0] = {source.Texture, Sampler};
		bindings[1] = {depth.Texture, Sampler};
		bindings[2] = {environment.IsValid() ? environment.Texture : State->Textures.Missing(), Sampler};
		uint32_t faceMask = 0;
		for (size_t index = 0; index < names.size(); ++index) {
			const auto owner = State->TextureContentOwnerWithFallback(
				names[index],
				State->ActiveContentOwner,
				State->ActiveLocalImageGraphWorld,
				State->ActiveImageGraphWorld
			);
			auto *texture = State->Textures.Find(names[index], owner);
			if (texture && names[index].IsValid()) faceMask |= 1u << index;
			bindings[index + 3] = {texture ? texture : State->Textures.Missing(), Sampler};
		}
		auto fallback =
			WeatherFallbackOf(lighting, environment.IsValid(), drawSky, drawClouds, linearDepth, faceMask);
		// Captures crop depth independently while colour retains its target's backing size.
		if (captureUniforms) fallback.Modes.x |= 128u;
		EnterNamedPass(context.Name);
		SDL_GPUColorTargetInfo colour{};
		colour.texture = target.Texture;
		colour.load_op = SDL_GPU_LOADOP_DONT_CARE;
		colour.store_op = SDL_GPU_STOREOP_STORE;
		colour.cycle = true;
		auto *pass = SDL_BeginGPURenderPass(Command, &colour, 1, nullptr);
		if (!pass) return false;
		SDL_BindGPUGraphicsPipeline(pass, State->WeatherFallbackPipeline);
		SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
		const auto &passUniforms = captureUniforms ? *captureUniforms : Uniforms;
		SDL_PushGPUFragmentUniformData(Command, 0, &passUniforms, sizeof(passUniforms));
		SDL_PushGPUFragmentUniformData(Command, 1, &fallback, sizeof(fallback));
		const SDL_GPUViewport viewport{0, 0, float(target.Width), float(target.Height), 0, 1};
		const SDL_Rect scissor{0, 0, int(target.Width), int(target.Height)};
		SDL_SetGPUViewport(pass, &viewport);
		SDL_SetGPUScissor(pass, &scissor);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		++Result.DrawCalls;
		core::Metrics::Count("render.weather.fragment_fallbacks", 1);
		StageHistoryWrites(context, Command);
		return true;
	}
}
