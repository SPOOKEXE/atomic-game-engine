#include <engine/core/Profiling.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/WeatherGround.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/scene/WeatherCompute.hpp>
#include <engine/weather/Runtime.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace engine::game {
	namespace {
		constexpr uint32_t Q16_ONE = 65536;
		constexpr uint64_t MAX_WEATHER_CATCH_UP_TICKS = 32;

		std::optional<float>
		GroundHeight(const ecs::Store &store, core::Vector3 origin, float maximumDistance) {
			const auto hit = physics::Raycast(
				store,
				{origin, {0, -1, 0}},
				maximumDistance,
				spatial::LayerMask::All(),
				ecs::NULL_ENTITY,
				false
			);
			return hit ? std::optional(hit->Position.Y) : std::nullopt;
		}

		bool MayImpact(const ecs::Store &store, ecs::Entity provider) {
			const auto *controls = scene::WeatherControlsOf(store, provider);
			const auto *clock = store.Get<scene::WeatherClock>(provider);
			if (!controls || !clock || !controls->Enabled || !scene::ValidWeatherClock(*clock) ||
				clock->Paused || clock->TimeScaleQ16 == 0)
				return false;
			const auto *state = store.Get<weather::WeatherRuntimeState>(provider);
			const bool ready = state && state->Ready && state->Epoch == clock->Epoch;
			const uint64_t completed = ready ? state->LastTick : clock->AnchorTick;
			if (clock->Tick <= completed) return false;
			const uint64_t ticks = std::min(MAX_WEATHER_CATCH_UP_TICKS, clock->Tick - completed);
			const double seconds =
				static_cast<double>(ticks) * clock->TimeScaleQ16 / Q16_ONE / clock->TickRate;
			const double charge = ready ? static_cast<double>(state->ChargeQ16) / Q16_ONE : 0;
			const double hail = ready ? static_cast<double>(state->HailGrowthQ16) / Q16_ONE : 0;
			const double roundingBound = static_cast<double>(ticks) / (2 * Q16_ONE);
			// Conservative unit-proxy bounds avoid rebuilding indexes before a discharge can exist.
			return (controls->LightningRate > 0 &&
					charge + seconds * controls->LightningRate * controls->StormIntensity + roundingBound >=
						1) ||
				   (controls->HailIntensity > 0 &&
					hail + seconds * controls->HailIntensity + roundingBound >= 1);
		}

		void PrepareWeatherGround(ecs::Store &store) {
			if (store.AdoptOnly()) return;
			const auto *cache = store.Resource<weather::WeatherRuntimeCache>();
			if (!cache) return;
			const bool queryNeeded =
				std::any_of(cache->Layers.begin(), cache->Layers.end(), [&](const auto &layer) {
					return layer.Compiled && MayImpact(store, layer.Provider);
				});
			if (!queryNeeded) return;
			ENGINE_PROFILE("weather ground index preparation");
			InstallWeatherGround(store);
			if (!store.HasResource<physics::PhysicsWorld>()) return;
			// Contacts publish after the solver's earlier index build. Refresh once before const event
			// queries.
			physics::SyncBroadphase(store);
		}
	}

	void InstallWeatherGround(ecs::Store &store) {
		if (store.AdoptOnly()) return;
		const auto *installed = store.Resource<weather::WeatherGroundQuery>();
		if (!installed || !installed->Height) store.SetResource(weather::WeatherGroundQuery{GroundHeight});
	}

	void RegisterWeatherGroundSystem(ecs::Scheduler &scheduler) {
		scheduler.Add("weather.ground", ecs::Phase::Animation, PrepareWeatherGround);
	}
}
