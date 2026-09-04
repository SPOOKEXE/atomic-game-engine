#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/Clock.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::script {

	void SetScriptTickRate(ecs::Store &store, double updatesPerSecond) {
		static const core::Name CLOCK_COMPONENT("script.ScriptClock");
		if (!ecs::Components::Find(CLOCK_COMPONENT).IsValid()) {
			return;
		}
		ScriptClock *clock = store.ResourceMutable<ScriptClock>();
		if (clock == nullptr) {
			store.SetResource(ScriptClock{});
			clock = store.ResourceMutable<ScriptClock>();
			if (clock == nullptr) {
				return;
			}
		}

		clock->Rate = std::isfinite(updatesPerSecond) ? std::max(updatesPerSecond, 0.0) : 0.0;
		clock->Accumulator = 0.0;
		clock->ObservedTick = std::numeric_limits<uint64_t>::max();
	}

	std::optional<float> TakeScriptUpdate(ecs::Store &store) {
		static const core::Name CLOCK_COMPONENT("script.ScriptClock");
		if (!ecs::Components::Find(CLOCK_COMPONENT).IsValid()) {
			return std::nullopt;
		}
		ScriptClock *clock = store.ResourceMutable<ScriptClock>();
		if (clock == nullptr) {
			return std::nullopt;
		}

		const ecs::WorldTime worldTime = store.Time();
		const float worldDelta = worldTime.Delta;
		if (!(clock->Rate > 0.0)) {
			return worldDelta;
		}

		const double interval = 1.0 / clock->Rate;
		if (clock->ObservedTick != worldTime.Tick) {
			clock->Accumulator += worldDelta;
			clock->ObservedTick = worldTime.Tick;
		}
		if (clock->Accumulator < interval) {
			return std::nullopt;
		}

		clock->Accumulator -= interval;
		return static_cast<float>(interval);
	}
}
