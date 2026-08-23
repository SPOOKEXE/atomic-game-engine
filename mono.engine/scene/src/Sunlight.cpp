#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::scene {

	namespace {
		core::Vector3 Normalised(core::Vector3 direction) {
			const float length = direction.Magnitude();
			return length > 0.0f ? direction / length : SUN_DIRECTION;
		}

		core::Vector3 SolarDirection(float clockTime, float latitude) {
			constexpr float DEGREES_TO_RADIANS = std::numbers::pi_v<float> / 180.0f;

			const float wrappedHours = clockTime - std::floor(clockTime / 24.0f) * 24.0f;
			const float hourAngle = (wrappedHours - 12.0f) * 15.0f * DEGREES_TO_RADIANS;
			const float clampedLatitude = std::clamp(latitude, -90.0f, 90.0f) * DEGREES_TO_RADIANS;

			const float cosineHour = std::cos(hourAngle);
			return Normalised(
				core::Vector3{
					-std::sin(hourAngle),
					-std::cos(clampedLatitude) * cosineHour,
					-std::sin(clampedLatitude) * cosineHour,
				}
			);
		}
	}

	WorldLighting LightingOf(const ecs::Store &store) {
		WorldLighting lighting;

		const ecs::ClassId lightingClass = ecs::Classes::Find(core::Name("Lighting"));
		const ecs::Entity service = ServiceOf(store, lightingClass);
		if (const LightingServiceComponent *authored = store.Get<LightingServiceComponent>(service)) {
			lighting.Direction = SolarDirection(authored->ClockTime, authored->GeographicLatitude);
			lighting.Ambient = authored->Ambient;
			lighting.OutdoorAmbient = authored->OutdoorAmbient;

			// The equinox altitude is the only daylight signal the two authored
			// solar properties provide. Below the horizon the directional term is
			// zero rather than an upward light from beneath the world.
			const float daylight = std::clamp(-lighting.Direction.Y, 0.0f, 1.0f);
			const float direct = std::max(authored->Brightness, 0.0f) * daylight;
			lighting.Direct = core::Color3{direct, direct, direct};

			lighting.FogColor = authored->FogColor;
			lighting.FogStart = std::max(authored->FogStart, 0.0f);
			lighting.FogEnd = std::max(authored->FogEnd, lighting.FogStart);
		}

		// **Resolved here rather than left to whoever draws**, for the reason
		// this function exists at all: a portal's far half is a copy of a world
		// lit by the same authored state, and a second resolver reading the
		// instance tree from inside a render pass would have to find the same
		// `Atmosphere` twice and could disagree about which one.
		lighting.EnvironmentState = EnvironmentOf(store);

		if (const Sun *override = store.Resource<Sun>()) {
			lighting.Direction = Normalised(override->Direction);
			lighting.Ambient = override->Ambient;
		}

		lighting.Direction = Normalised(lighting.Direction);
		return lighting;
	}

	Sun SunOf(const ecs::Store &store) {
		const WorldLighting lighting = LightingOf(store);
		Sun sun;
		sun.Direction = lighting.Direction;
		sun.Ambient = lighting.Ambient;

		return sun;
	}
}
