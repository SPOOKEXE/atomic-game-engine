#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/WeatherEffects.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/weather/Runtime.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::render {
	namespace {
		constexpr float PRECIPITATION_RADIUS_METRES = 64.0f;
		constexpr float PRECIPITATION_HEIGHT_METRES = 48.0f;
		constexpr float CAMERA_TILE_METRES = 16.0f;
		constexpr double LIGHTNING_SECONDS = 0.3;
		constexpr float FOG_HEIGHT_METRES = 40.0f;
		constexpr float GEOMETRY_RAY_METRES = 8192.0f;

		bool Finite(core::Vector3 value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		uint64_t GeometrySignature(std::span<const scene::DrawInstance> geometry) {
			uint64_t signature = 1469598103934665603ull;
			const auto add = [&](float value) {
				signature = (signature ^ std::bit_cast<uint32_t>(value)) * 1099511628211ull;
			};
			for (const auto &instance : geometry) {
				if (instance.Transparency >= 0.5f) continue;
				const auto turn = instance.Frame.Rotation();
				add(instance.Frame.Position.X);
				add(instance.Frame.Position.Y);
				add(instance.Frame.Position.Z);
				add(turn.x);
				add(turn.y);
				add(turn.z);
				add(turn.w);
				add(instance.HalfExtent.X);
				add(instance.HalfExtent.Y);
				add(instance.HalfExtent.Z);
			}
			return signature;
		}

		std::optional<float> WeatherBoundHit(
			const scene::DrawInstance &instance,
			core::Vector3 originMetres,
			core::Vector3 velocityMetresPerSecond,
			float maximumSeconds
		) {
			if (instance.Transparency >= 0.5f || !Finite(instance.HalfExtent) || instance.HalfExtent.X <= 0 ||
				instance.HalfExtent.Y <= 0 || instance.HalfExtent.Z <= 0)
				return std::nullopt;
			const auto point = instance.Frame.PointToObjectSpace(originMetres);
			const auto velocity = instance.Frame.VectorToObjectSpace(velocityMetresPerSecond);
			const std::array<float, 3> offsets{point.X, point.Y, point.Z};
			const std::array<float, 3> speeds{velocity.X, velocity.Y, velocity.Z};
			const std::array<float, 3> half{
				instance.HalfExtent.X, instance.HalfExtent.Y, instance.HalfExtent.Z
			};
			float enter = 0, leave = maximumSeconds;
			bool intersects = true;
			for (size_t axis = 0; axis < 3; ++axis) {
				if (std::abs(speeds[axis]) < 0.000001f) {
					if (std::abs(offsets[axis]) > half[axis]) intersects = false;
					continue;
				}
				float first = (-half[axis] - offsets[axis]) / speeds[axis];
				float second = (half[axis] - offsets[axis]) / speeds[axis];
				if (first > second) std::swap(first, second);
				enter = std::max(enter, first);
				leave = std::min(leave, second);
				if (leave < enter) intersects = false;
			}
			return intersects && enter <= leave ? std::optional<float>(enter) : std::nullopt;
		}

		std::optional<float> NearbyWeatherHit(
			std::span<const scene::DrawInstance *const> geometry,
			core::Vector3 origin,
			core::Vector3 velocity,
			float maximumSeconds
		) {
			std::optional<float> nearest;
			for (const auto *instance : geometry) {
				if (const auto hit =
						WeatherBoundHit(*instance, origin, velocity, nearest.value_or(maximumSeconds)))
					nearest = hit;
			}
			return nearest;
		}

		void AppendLightning(
			const weather::StormEvent &event,
			const scene::WeatherClock &clock,
			core::Vector3 eye,
			effects::RibbonBuffer &ribbons,
			std::vector<SceneLight> &lights
		) {
			if (event.Kind != weather::StormEventKind::Lightning || event.Id.Epoch != clock.Epoch ||
				clock.TickRate == 0)
				return;
			const auto age = weather::WeatherEventAgeSeconds(
				event, clock.ElapsedSeconds, clock.ElapsedFractionQ32, clock.Fraction / clock.TickRate
			);
			// A copied Q32 timestamp can fall one quantum below the lifetime boundary.
			if (!age || *age + 1.0 / 4294967296.0 >= LIGHTNING_SECONDS) return;
			std::array<weather::LightningSegment, weather::MAX_LIGHTNING_SEGMENTS> segments{};
			const size_t count = weather::BuildLightningSegments(event, segments);
			const float fade = 1.0f - float(*age / LIGHTNING_SECONDS);
			const uint32_t colour = 0x00FFF3E5u | (uint32_t(std::lround(fade * 255.0f)) << 24u);
			for (size_t index = 0; index < count; ++index) {
				const auto &segment = segments[index];
				const auto midpoint = (segment.StartMetres + segment.EndMetres) * 0.5f;
				const auto tangent = segment.EndMetres - segment.StartMetres;
				auto side = tangent.Cross(eye - midpoint);
				const float length = side.Magnitude();
				side = length > 0.0001f ? side / length : core::Vector3{1, 0, 0};
				side = side * (segment.Branch ? 0.15f : 0.4f);
				const auto first = static_cast<uint32_t>(ribbons.Vertices.size());
				ribbons.Vertices.push_back({segment.StartMetres - side, {0, 0}, colour});
				ribbons.Vertices.push_back({segment.StartMetres + side, {0, 1}, colour});
				ribbons.Vertices.push_back({segment.EndMetres - side, {1, 0}, colour});
				ribbons.Vertices.push_back({segment.EndMetres + side, {1, 1}, colour});
				effects::RibbonRun run;
				run.First = first;
				run.Count = 4;
				run.Additive = true;
				run.FaceCamera = true;
				ribbons.Runs.push_back(run);
			}
			if (count != 0 && lights.size() < MAX_SCENE_LIGHTS) {
				SceneLight flash;
				flash.Position = event.EndMetres + core::Vector3{0, 8, 0};
				flash.Range = 512;
				flash.Colour = {15 * fade, 16 * fade, 18 * fade};
				lights.push_back(flash);
			}
		}
	}

	std::optional<float> WeatherGeometryHit(
		std::span<const scene::DrawInstance> geometry,
		core::Vector3 originMetres,
		core::Vector3 velocityMetresPerSecond,
		float maximumSeconds
	) {
		if (!Finite(originMetres) || !Finite(velocityMetresPerSecond) || !std::isfinite(maximumSeconds) ||
			maximumSeconds <= 0)
			return std::nullopt;
		std::optional<float> nearest;
		for (const auto &instance : geometry) {
			if (const auto hit = WeatherBoundHit(
					instance, originMetres, velocityMetresPerSecond, nearest.value_or(maximumSeconds)
				))
				nearest = hit;
		}
		return nearest;
	}

	void CollectWeatherEffects(
		ecs::Store &store,
		const View &view,
		WeatherEffectsFrame &frame,
		effects::RibbonBuffer &ribbons,
		std::vector<SceneLight> &lights
	) {
		ENGINE_PROFILE_CAT("weather consumers collect", core::ProfileCategory::Render);
		const auto &environment = view.Lighting.EnvironmentState;
		if (!environment.HasWeatherCompute || !environment.Weather.Enabled) {
			frame.Precipitation.reset();
			frame.Sample.reset();
			frame.Fog.reset();
			frame.HasLighting = false;
			return;
		}
		const auto &controls = environment.Weather;
		const auto &clock = environment.Clock;
		const auto eye = view.VisibilityCameraFrame().Position;
		if (!Finite(eye)) return;
		const auto *events = store.Get<weather::WeatherEvents>(environment.CloudProvider);
		const bool needsAir = controls.PrecipitationRate > 0 || controls.FogStrength > 0 ||
							  controls.AtmosphereDensityStrength > 0 || controls.AtmosphereHazeStrength > 0 ||
							  controls.AtmosphereGlareSuppression > 0;
		if (!needsAir) {
			frame.Precipitation.reset();
			frame.Sample.reset();
			frame.Fog.reset();
			frame.HasLighting = false;
			if (events)
				for (const auto &event : events->Events)
					AppendLightning(event, clock, eye, ribbons, lights);
			return;
		}
		const core::Vector3 centre{
			std::floor(eye.X / CAMERA_TILE_METRES) * CAMERA_TILE_METRES,
			std::floor(eye.Y / CAMERA_TILE_METRES) * CAMERA_TILE_METRES + PRECIPITATION_HEIGHT_METRES * 0.5f,
			std::floor(eye.Z / CAMERA_TILE_METRES) * CAMERA_TILE_METRES
		};
		const uint64_t geometry = GeometrySignature(view.Instances);
		// Half-second emission maintenance stays responsive to geometry, teleports
		// and definition changes; particle motion itself remains a device step.
		const uint64_t maintenanceTicks = std::max(1u, clock.TickRate / 2u);
		const bool rebuild = !frame.HasLighting || frame.World != store.Identity() ||
							 frame.Epoch != clock.Epoch || frame.Definition != controls.DefinitionRevision ||
							 frame.Geometry != geometry || frame.CentreMetres != centre ||
							 clock.Tick < frame.Tick || clock.Tick - frame.Tick >= maintenanceTicks;
		if (rebuild) {
			const auto ground = WeatherGeometryHit(
				view.Instances, eye + core::Vector3{0, 0.1f, 0}, {0, -1, 0}, GEOMETRY_RAY_METRES
			);
			const float groundHeight = ground ? eye.Y + 0.1f - *ground : 0.0f;
			frame.Sample = weather::QueryWeather(store, environment.CloudProvider, eye, groundHeight);
			const auto &sample = frame.Sample;
			frame.Precipitation.reset();
			frame.Fog.reset();
			frame.HasLighting = sample.has_value();
			if (sample) {
				// Fog exists below an observer flying above it too. Sample the
				// cooled surface air separately from the eye's atmospheric response.
				const auto surfaceAir = controls.FogStrength > 0
											? weather::QueryWeather(
												  store,
												  environment.CloudProvider,
												  core::Vector3{centre.X, groundHeight + 1.0f, centre.Z},
												  groundHeight
											  )
											: std::optional<weather::ConsumerSample>{};
				const float fogHeight = std::min(FOG_HEIGHT_METRES, controls.BaseHeightMetres - groundHeight);
				if (surfaceAir && surfaceAir->FogExtinctionPerMetre > 0 && fogHeight > 0) {
					scene::VolumeState fog;
					fog.Enabled = true;
					fog.Frame.Position = {centre.X, groundHeight + fogHeight * 0.5f, centre.Z};
					fog.HalfExtent = {
						PRECIPITATION_RADIUS_METRES * 2, fogHeight * 0.5f, PRECIPITATION_RADIUS_METRES * 2
					};
					fog.Colour = environment.Air.Colour;
					fog.Density = 1;
					fog.Extinction = surfaceAir->FogExtinctionPerMetre;
					fog.NoiseStrength = 0;
					fog.Falloff = 0.1f;
					frame.Fog = fog;
				}
				auto emission = *sample;
				// Reusable source samples keep one purpose seed across maintenance.
				// Accepted weather ticks change rates and wind, never the particle id.
				emission.Precipitation.SpawnSeed = weather::Hash(
					controls.Seed ^ weather::WeatherLayerKey(controls.LayerName.Text()) ^ 0x50524543u
				);
				auto field = weather::BuildPrecipitationParticles(
					emission,
					{PRECIPITATION_RADIUS_METRES, PRECIPITATION_HEIGHT_METRES, PRECIPITATION_RADIUS_METRES}
				);
				if (field && field->Enabled) {
					// One broad pass bounds the ninety-six trajectory queries. Capacity
					// exhaustion suppresses cosmetic drops rather than assuming no roof.
					std::array<const scene::DrawInstance *, 256> candidates{};
					size_t candidateCount = 0;
					bool overflow = false;
					for (const auto &instance : view.Instances) {
						if (instance.Transparency >= 0.5f) continue;
						const float radius = instance.HalfExtent.Magnitude();
						if (std::abs(instance.Frame.Position.X - centre.X) >
								radius + PRECIPITATION_RADIUS_METRES ||
							std::abs(instance.Frame.Position.Z - centre.Z) >
								radius + PRECIPITATION_RADIUS_METRES)
							continue;
						if (candidateCount == candidates.size()) {
							overflow = true;
							break;
						}
						candidates[candidateCount++] = &instance;
					}
					const std::span<const scene::DrawInstance *const> nearby(
						candidates.data(), candidateCount
					);
					core::Metrics::Count("weather.effects.occlusion_candidates", candidateCount);
					if (overflow) core::Metrics::Count("weather.effects.occlusion_capacity_refused", 1);
					std::erase_if(field->SpawnSamples, [&](scene::GpuParticleSpawnSample &spawn) {
						const auto world = centre + spawn.Position;
						if (overflow || NearbyWeatherHit(nearby, world, {0, 1, 0}, GEOMETRY_RAY_METRES))
							return true;
						if (const auto hit = NearbyWeatherHit(nearby, world, spawn.Velocity, spawn.Lifetime))
							spawn.Lifetime = std::min(spawn.Lifetime, std::max(0.0f, *hit - 0.01f));
						return spawn.Lifetime < 0.01f;
					});
					if (!field->SpawnSamples.empty()) {
						frame.Precipitation.emplace();
						frame.Precipitation->Field = std::move(*field);
						frame.Precipitation->Frame.Position = centre;
						frame.Precipitation->Source = environment.CloudProvider;
						frame.Precipitation->DefinitionRevision = ++frame.Signature;
					}
				}
			}
			frame.World = store.Identity();
			frame.Epoch = clock.Epoch;
			frame.Tick = clock.Tick;
			frame.Definition = controls.DefinitionRevision;
			frame.Geometry = geometry;
			frame.CentreMetres = centre;
			core::Metrics::Count("weather.effects.rebuild", 1);
		}
		// Authored lighting and camera-selected local volumes can change between
		// emission maintenance passes. Compose them freshly from the current copy.
		frame.Lighting = view.Lighting;
		if (frame.Sample) {
			weather::AtmosphereResponse response;
			response.DensityStrength = controls.AtmosphereDensityStrength;
			response.HazeStrength = controls.AtmosphereHazeStrength;
			response.GlareSuppression = controls.AtmosphereGlareSuppression;
			response.DensityPrecedence = response.DensityStrength > 0 ? weather::ResponsePrecedence::Weather
																	  : weather::ResponsePrecedence::Authored;
			response.HazePrecedence = response.HazeStrength > 0 ? weather::ResponsePrecedence::Weather
																: weather::ResponsePrecedence::Authored;
			response.GlarePrecedence = response.GlareSuppression > 0 ? weather::ResponsePrecedence::Weather
																	 : weather::ResponsePrecedence::Authored;
			frame.Lighting.EnvironmentState.Air =
				weather::ApplyAtmosphereResponse(environment.Air, *frame.Sample, response);
			frame.Lighting.EnvironmentState.HasAtmosphere =
				environment.HasAtmosphere || response.DensityStrength > 0 || response.HazeStrength > 0 ||
				response.GlareSuppression > 0;
		}
		if (frame.Fog && frame.Lighting.VolumeCount < scene::MAX_SCENE_VOLUMES)
			frame.Lighting.Volumes[frame.Lighting.VolumeCount++] = *frame.Fog;
		if (frame.Precipitation)
			frame.Precipitation->Seconds =
				clock.TickRate == 0 ? 0 : (double(clock.Tick) + clock.Fraction) / clock.TickRate;
		if (events) {
			for (const auto &event : events->Events)
				AppendLightning(event, clock, eye, ribbons, lights);
		}
	}
}
