#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/LightingEffects.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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

		// Capture lighting requires a finite, strictly positive fog interval.
		// The next representable float preserves Start except at the largest float.
		void ResolveFogRange(float authoredStart, float authoredEnd, float &start, float &end) {
			const float maximum = std::numeric_limits<float>::max();
			const float largestStart = std::nextafter(maximum, 0.0f);
			start = std::isnan(authoredStart) ? 0.0f : std::clamp(authoredStart, 0.0f, largestStart);
			end = std::isnan(authoredEnd) ? 0.0f : std::clamp(authoredEnd, 0.0f, maximum);
			if (end <= start) end = std::nextafter(start, maximum);
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

			lighting.RenderFeatures = authored->RenderFeatures;
		}

		if (service != ecs::NULL_ENTITY) {
			const ecs::ClassId bloomClass = ecs::Classes::Find(core::Name("BloomEffect"));
			const ecs::ClassId depthClass = ecs::Classes::Find(core::Name("DepthOfFieldEffect"));
			const ecs::ClassId raysClass = ecs::Classes::Find(core::Name("GodRaysEffect"));
			const ecs::ClassId fogClass = ecs::Classes::Find(core::Name("FogEffect"));
			bool hasBloom = false;
			bool hasDepthOfField = false;
			bool hasGodRays = false;
			bool hasFog = false;
			// The first provider of each class owns that effect, even when disabled.
			// A fresh walk also observes mutation, removal and reparenting this frame.
			store.EachDescendant(service, [&](ecs::Entity descendant) {
				if (!hasBloom && store.IsA(descendant, bloomClass)) {
					if (const BloomEffect *effect = store.Get<BloomEffect>(descendant)) {
						hasBloom = true;
						if (effect->Enabled) {
							lighting.BloomIntensity = std::max(effect->Intensity, 0.0f);
							lighting.BloomThreshold = std::max(effect->Threshold, 0.0f);
							lighting.BloomRadius = std::max(effect->Radius, 0.0f);
						}
					}
				}
				if (!hasDepthOfField && store.IsA(descendant, depthClass)) {
					if (const DepthOfFieldEffect *effect = store.Get<DepthOfFieldEffect>(descendant)) {
						hasDepthOfField = true;
						if (effect->Enabled) {
							lighting.DepthOfFieldIntensity = std::clamp(effect->Intensity, 0.0f, 1.0f);
							lighting.DepthOfFieldFocusDistance = std::max(effect->FocusDistance, 0.0f);
							lighting.DepthOfFieldFocusRange = std::max(effect->FocusRange, 0.0f);
							lighting.DepthOfFieldRadius = std::max(effect->Radius, 0.0f);
						}
					}
				}
				if (!hasGodRays && store.IsA(descendant, raysClass)) {
					if (const GodRaysEffect *effect = store.Get<GodRaysEffect>(descendant)) {
						hasGodRays = true;
						if (effect->Enabled) {
							lighting.GodRayIntensity = std::max(effect->Intensity, 0.0f);
							lighting.GodRayThreshold = std::max(effect->Threshold, 0.0f);
							lighting.GodRayRadius = std::max(effect->Radius, 0.0f);
						}
					}
				}
				if (!hasFog && store.IsA(descendant, fogClass)) {
					if (const FogEffect *effect = store.Get<FogEffect>(descendant)) {
						hasFog = true;
						if (effect->Enabled) {
							lighting.FogColor = effect->Color;
							ResolveFogRange(effect->Start, effect->End, lighting.FogStart, lighting.FogEnd);
						}
					}
				}
			});
		}

		// **Resolved here rather than left to whoever draws**, for the reason
		// this function exists at all: a portal's far half is a copy of a world
		// lit by the same authored state, and a second resolver reading the
		// instance tree from inside a render pass would have to find the same
		// `Atmosphere` twice and could disagree about which one.
		lighting.EnvironmentState = EnvironmentOf(store);
		lighting.VolumeCount = ResolveVolumes(store, lighting.Volumes);
		lighting.ShaderLensCount = ResolveShaderLenses(store, lighting.ShaderLenses);

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
