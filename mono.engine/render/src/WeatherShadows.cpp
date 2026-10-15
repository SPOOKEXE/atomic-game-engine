#include "WeatherShadows.hpp"

#include "WeatherGpu.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/weather/Consumers.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::render {
	namespace {
		constexpr float MINIMUM_SUN_HEIGHT = .025f;
		constexpr float FULL_SUN_HEIGHT = .12f;
		constexpr float MAXIMUM_FOOTPRINT_RATIO = 4.0f;

		uint64_t Mix(uint64_t signature, float value) {
			return scene::MixSignature(signature, std::bit_cast<uint32_t>(value));
		}

		struct WeatherShadowMapUniforms {
			WeatherShadowLightingUniforms Lighting;
			glm::vec4 Minimum;
			glm::vec4 Maximum;
			glm::uvec4 Quality;
			glm::vec4 OpticalError;
			WeatherGpuUniforms Field;
		};
		static_assert(sizeof(WeatherShadowMapUniforms) == 352);
	}

	WeatherShadowPlan BuildWeatherShadowPlan(
		const scene::WeatherCompute &controls,
		const WeatherGpuInputs &inputs,
		core::Vector3 sunDirection,
		float appearanceDensity,
		uint64_t timelineRevision,
		std::span<const WeatherResidencyView> views
	) {
		WeatherShadowPlan plan;
		if (!controls.Enabled || !controls.ShadowsEnabled || !(controls.ShadowStrength > 0) ||
			!(controls.ShadowDistanceMetres > 0) || !(appearanceDensity > 0) || views.empty())
			return plan;
		const float length = sunDirection.Magnitude();
		if (!(length > 0) || !std::isfinite(length)) return plan;
		const core::Vector3 toSun = sunDirection * (-1.0f / length);
		if (!(toSun.Y > MINIMUM_SUN_HEIGHT)) return plan;

		core::Vector3 centre{};
		float minimumHeight = views.front().View.CameraPositionMetres.Y;
		for (const auto &view : views) {
			const auto &position = view.View.CameraPositionMetres;
			if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(position.Z))
				return plan;
			centre = centre + position * (1.0f / static_cast<float>(views.size()));
			minimumHeight = std::min(minimumHeight, position.Y);
		}
		float viewSpread = 0;
		for (const auto &view : views) {
			const auto offset = view.View.CameraPositionMetres - centre;
			viewSpread = std::max(viewSpread, std::max(std::abs(offset.X), std::abs(offset.Z)));
		}
		minimumHeight = std::floor(minimumHeight / controls.CellSizeMetres) * controls.CellSizeMetres;
		const float radius =
			std::ceil((controls.ShadowDistanceMetres + viewSpread) / controls.CellSizeMetres) *
			controls.CellSizeMetres;
		const float cloudBottom = controls.BaseHeightMetres;
		const float cloudTop = cloudBottom + controls.ThicknessMetres;
		// Receiver elevation is projected along the same sunlight ray as the map.
		// Limit the low-sun footprint and fade the whole contribution before it becomes unbounded.
		const float casterShift = std::max(cloudTop - minimumHeight, 0.0f) *
								  std::sqrt(toSun.X * toSun.X + toSun.Z * toSun.Z) / toSun.Y;
		const float footprintCap = radius * MAXIMUM_FOOTPRINT_RATIO;
		const float footprintFade = std::clamp(2.0f - casterShift / footprintCap, 0.0f, 1.0f);
		const float angleFade =
			std::clamp((toSun.Y - MINIMUM_SUN_HEIGHT) / (FULL_SUN_HEIGHT - MINIMUM_SUN_HEIGHT), 0.0f, 1.0f);
		if (!(footprintFade > 0 && angleFade > 0)) return plan;
		plan.Resolution = std::bit_floor(std::clamp(controls.ShadowResolution, 32u, 2048u));
		plan.Cascades = std::clamp(controls.ShadowCascades, 1u, WEATHER_SHADOW_CASCADES);
		const float bottomTravel = std::max(cloudBottom - minimumHeight, 0.0f) / toSun.Y;
		const float mapCentreX = centre.X + toSun.X * bottomTravel;
		const float mapCentreZ = centre.Z + toSun.Z * bottomTravel;
		const float mapRadius =
			std::ceil((radius + std::min(casterShift, footprintCap)) / controls.CellSizeMetres) *
			controls.CellSizeMetres;
		for (uint32_t cascade = 0; cascade < plan.Cascades; ++cascade) {
			const float cascadeRadius = std::ldexp(mapRadius, -int(plan.Cascades - cascade - 1));
			const float texel = cascadeRadius * 2.0f / plan.Resolution;
			plan.Lighting.Regions[cascade] = {
				std::floor(mapCentreX / texel) * texel,
				std::floor(mapCentreZ / texel) * texel,
				cascadeRadius,
				.5f / plan.Resolution
			};
		}
		plan.Lighting.Controls = {
			std::clamp(controls.ShadowStrength, 0.0f, 1.0f), cloudBottom, cloudTop, float(plan.Cascades)
		};
		plan.Lighting.ToSun = {toSun.X, toSun.Y, toSun.Z, radius};
		// The distance fade follows the same snapped domain as the retained map.
		const auto &outer = plan.Lighting.Regions[plan.Cascades - 1];
		plan.Lighting.Receiver = {
			outer.x - toSun.X * bottomTravel,
			outer.y - toSun.Z * bottomTravel,
			minimumHeight,
			angleFade * footprintFade
		};
		const float heightTravel = controls.ThicknessMetres / toSun.Y;
		const float padding = controls.CellSizeMetres * 2.0f;
		plan.Casters = {
			{outer.x - outer.z + std::min(0.0f, toSun.X * heightTravel) - padding,
			 cloudBottom - padding,
			 outer.y - outer.z + std::min(0.0f, toSun.Z * heightTravel) - padding},
			{outer.x + outer.z + std::max(0.0f, toSun.X * heightTravel) + padding,
			 cloudTop + padding,
			 outer.y + outer.z + std::max(0.0f, toSun.Z * heightTravel) + padding}
		};
		const uint64_t interval = std::max<uint64_t>(
			1, uint64_t(std::ceil(std::max(controls.ShadowUpdateSeconds, 0.0f) * controls.TickRate))
		);
		// Changes to lighting, domain and definitions invalidate immediately. Motion is rate-limited.
		auto definitionWords = inputs.Uniforms;
		definitionWords.Time = {0, 0, 0, inputs.Time.Revision};
		definitionWords.Phase = {};
		uint64_t signature = scene::MixSignature(timelineRevision, WeatherSignature(definitionWords));
		signature = scene::MixSignature(signature, inputs.Compiled.Definition.Seed);
		signature = scene::MixSignature(signature, inputs.Compiled.Definition.LayerKey);
		signature = scene::MixSignature(signature, inputs.Time.Revision);
		signature = scene::MixSignature(signature, inputs.Time.PhaseTick / interval);
		signature = scene::MixSignature(signature, plan.Resolution);
		signature = Mix(signature, appearanceDensity);
		signature = Mix(signature, controls.CondensateScale);
		signature = Mix(signature, controls.LodError);
		for (const auto &region : plan.Lighting.Regions)
			for (uint32_t lane = 0; lane < 4; ++lane)
				signature = Mix(signature, region[lane]);
		for (const auto vector : {plan.Lighting.Controls, plan.Lighting.ToSun, plan.Lighting.Receiver})
			for (uint32_t lane = 0; lane < 4; ++lane)
				signature = Mix(signature, vector[lane]);
		plan.Signature = signature;
		plan.Enabled = true;
		return plan;
	}

	size_t AppendWeatherShadowDemand(
		std::span<WeatherResidencyView> views, size_t count, const WeatherShadowPlan &plan, uint64_t layerKey
	) {
		if (!plan.Enabled) return count;
		WeatherResidencyView demand;
		demand.View.Key = scene::MixSignature(layerKey, 0x636c6f7564736864ull);
		demand.View.CameraPositionMetres = plan.Casters.Centre();
		demand.ProjectionPixelsPerRadian = float(plan.Resolution);
		const auto extent = plan.Casters.Size();
		demand.MaximumDistanceMetres = .5f * extent.Magnitude();
		if (count < views.size() && count < weather::MAXIMUM_RESIDENCY_VIEWS) {
			views[count] = demand;
			return count + 1;
		} else {
			core::Metrics::Count("render.weather.shadow_refused_demands", 1);
			return count;
		}
	}

	bool RecordWeatherShadows(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUBuffer *cells,
		SDL_GPUBuffer *pages,
		SDL_GPUTexture *destination,
		const WeatherShadowPlan &plan,
		const WeatherGpuInputs &inputs,
		const scene::WeatherCompute &controls,
		float appearanceDensity,
		SDL_GPUBuffer *clock
	) {
		ENGINE_PROFILE_CAT("weather cloud shadows", core::ProfileCategory::Render);
		if (!command || !pipeline || !cells || !pages || !destination || !plan.Enabled) return false;
		WeatherShadowMapUniforms uniforms{
			.Lighting = plan.Lighting,
			.Minimum =
				{-controls.ExtentMetres.X * .5f,
				 controls.BaseHeightMetres,
				 -controls.ExtentMetres.Z * .5f,
				 controls.CellSizeMetres},
			.Maximum =
				{controls.ExtentMetres.X * .5f,
				 controls.BaseHeightMetres + controls.ThicknessMetres,
				 controls.ExtentMetres.Z * .5f,
				 weather::ExtinctionPerMetre(controls.CondensateScale) * appearanceDensity},
			.Quality = {plan.Resolution, plan.Cascades, 64, inputs.Stateful ? 1u : 0u},
			.OpticalError =
				{controls.LodError,
				 weather::ExtinctionPerMetre(controls.CondensateScale) * appearanceDensity *
					 controls.ThicknessMetres / plan.Lighting.ToSun.y,
				 0,
				 0},
			.Field = inputs.Uniforms
		};
		if (clock && uniforms.Field.Phase.z != UINT32_MAX) {
			uniforms.Field.Time = {};
			uniforms.Field.Phase = {0, 0, 1, uniforms.Field.Phase.w};
		}
		SDL_GPUStorageTextureReadWriteBinding output{};
		output.texture = destination;
		output.cycle = true;
		auto *pass = SDL_BeginGPUComputePass(command, &output, 1, nullptr, 0);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, pipeline);
		SDL_GPUBuffer *buffers[]{cells, pages, clock ? clock : pages};
		SDL_BindGPUComputeStorageBuffers(pass, 0, buffers, 3);
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (plan.Resolution + 7) / 8, (plan.Resolution * plan.Cascades + 7) / 8, 1);
		SDL_EndGPUComputePass(pass);
		core::Metrics::Count("render.weather.shadow_dispatches", 1);
		core::Metrics::Count(
			"render.weather.shadow_texels", uint64_t(plan.Resolution) * plan.Resolution * plan.Cascades
		);
		return true;
	}
}
