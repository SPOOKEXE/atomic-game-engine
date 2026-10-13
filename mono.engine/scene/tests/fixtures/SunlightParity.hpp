#pragma once

// Independent equinox oracle shared by the resolver suite and benchmark
// preflight. It uses separate worlds and never advances the measured world.
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/LightingEffects.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace sunlight_fixture {
	inline void Require(bool condition) {
		if (!condition) throw std::runtime_error("sunlight output differs from independent oracle");
	}
	inline void Near(float actual, double expected) {
		Require(std::isfinite(actual) && std::abs(static_cast<double>(actual) - expected) < 0.00001);
	}
	inline void Colour(engine::core::Color3 actual, engine::core::Color3 expected) {
		Require(actual == expected);
	}
	inline void Check(
		const engine::scene::WorldLighting &actual,
		const engine::scene::LightingServiceComponent *authored,
		const engine::scene::FogEffect *fog = nullptr,
		const engine::scene::BloomEffect *bloom = nullptr,
		const engine::scene::DepthOfFieldEffect *depth = nullptr,
		const engine::scene::GodRaysEffect *rays = nullptr
	) {
		using namespace engine::scene;
		WorldLighting expected;
		if (authored) {
			// Double precision analytical equinox arc, independently of the float
			// angle construction and two normalisation passes in the resolver.
			const double hours = static_cast<double>(authored->ClockTime);
			const double phase = (hours - 24.0 * std::floor(hours / 24.0) - 12.0) * std::numbers::pi / 12.0;
			const double latitude =
				std::clamp(static_cast<double>(authored->GeographicLatitude), -90.0, 90.0) *
				std::numbers::pi / 180.0;
			Near(actual.Direction.X, -std::sin(phase));
			Near(actual.Direction.Y, -std::cos(latitude) * std::cos(phase));
			Near(actual.Direction.Z, -std::sin(latitude) * std::cos(phase));
			Near(actual.Direction.Magnitude(), 1.0);
			const double direct = std::max(static_cast<double>(authored->Brightness), 0.0) *
								  std::max(std::cos(latitude) * std::cos(phase), 0.0);
			Near(actual.Direct.R, direct);
			Near(actual.Direct.G, direct);
			Near(actual.Direct.B, direct);
			expected.Ambient = authored->Ambient;
			expected.OutdoorAmbient = authored->OutdoorAmbient;
			expected.RenderFeatures = authored->RenderFeatures;
		} else {
			const double length = std::sqrt(
				double(SUN_DIRECTION.X) * SUN_DIRECTION.X + double(SUN_DIRECTION.Y) * SUN_DIRECTION.Y +
				double(SUN_DIRECTION.Z) * SUN_DIRECTION.Z
			);
			Near(actual.Direction.X, SUN_DIRECTION.X / length);
			Near(actual.Direction.Y, SUN_DIRECTION.Y / length);
			Near(actual.Direction.Z, SUN_DIRECTION.Z / length);
			Colour(actual.Direct, expected.Direct);
		}
		if (fog != nullptr && fog->Enabled) {
			expected.FogColor = fog->Color;
			const float maximum = std::numeric_limits<float>::max();
			const float largestStart = std::nextafter(maximum, 0.0f);
			expected.FogStart = std::isnan(fog->Start) ? 0.0f : std::clamp(fog->Start, 0.0f, largestStart);
			expected.FogEnd = std::isnan(fog->End) ? 0.0f : std::clamp(fog->End, 0.0f, maximum);
			if (expected.FogEnd <= expected.FogStart)
				expected.FogEnd = std::nextafter(expected.FogStart, maximum);
		}
		if (bloom != nullptr && bloom->Enabled) {
			expected.BloomIntensity = std::max(bloom->Intensity, 0.0f);
			expected.BloomThreshold = std::max(bloom->Threshold, 0.0f);
			expected.BloomRadius = std::max(bloom->Radius, 0.0f);
		}
		if (depth != nullptr && depth->Enabled) {
			expected.DepthOfFieldIntensity = std::clamp(depth->Intensity, 0.0f, 1.0f);
			expected.DepthOfFieldFocusDistance = std::max(depth->FocusDistance, 0.0f);
			expected.DepthOfFieldFocusRange = std::max(depth->FocusRange, 0.0f);
			expected.DepthOfFieldRadius = std::max(depth->Radius, 0.0f);
		}
		if (rays != nullptr && rays->Enabled) {
			expected.GodRayIntensity = std::max(rays->Intensity, 0.0f);
			expected.GodRayThreshold = std::max(rays->Threshold, 0.0f);
			expected.GodRayRadius = std::max(rays->Radius, 0.0f);
		}
		Colour(actual.Ambient, expected.Ambient);
		Colour(actual.OutdoorAmbient, expected.OutdoorAmbient);
		Colour(actual.FogColor, expected.FogColor);
		Require(
			actual.RenderFeatures.Enable == expected.RenderFeatures.Enable &&
			actual.RenderFeatures.Disable == expected.RenderFeatures.Disable
		);
		const std::array actualTerms{
			actual.FogStart,
			actual.FogEnd,
			actual.BloomThreshold,
			actual.BloomIntensity,
			actual.BloomRadius,
			actual.DepthOfFieldIntensity,
			actual.DepthOfFieldFocusDistance,
			actual.DepthOfFieldFocusRange,
			actual.DepthOfFieldRadius,
			actual.GodRayIntensity,
			actual.GodRayThreshold,
			actual.GodRayRadius
		};
		const std::array expectedTerms{
			expected.FogStart,
			expected.FogEnd,
			expected.BloomThreshold,
			expected.BloomIntensity,
			expected.BloomRadius,
			expected.DepthOfFieldIntensity,
			expected.DepthOfFieldFocusDistance,
			expected.DepthOfFieldFocusRange,
			expected.DepthOfFieldRadius,
			expected.GodRayIntensity,
			expected.GodRayThreshold,
			expected.GodRayRadius
		};
		Require(actualTerms == expectedTerms);
		// These worlds deliberately contain no environment providers or placed
		// effects. Empty selections and time are part of their expected output.
		const auto &environment = actual.EnvironmentState;
		Require(actual.VolumeCount == 0 && actual.ShaderLensCount == 0);
		Require(
			environment.Skybox == SkyboxSource::None && !environment.HasAtmosphere &&
			!environment.HasClouds && !environment.HasAtmosphereCompute && !environment.HasCloudCompute
		);
		Require(
			environment.CloudTime == 0.0 && environment.Air.Density == 0.0f &&
			!environment.CloudLayer.Enabled && !environment.AirCompute.Enabled &&
			!environment.CloudVolume.Enabled
		);
	}
	inline void Verify() {
		using namespace engine;
		scene::RegisterSceneClasses();
		ecs::Store empty("sunlight.oracle.empty");
		Check(scene::LightingOf(empty), nullptr);
		ecs::Store furnished("sunlight.oracle.defaults");
		scene::InstallServices(furnished);
		auto *defaults =
			furnished.GetMutable<scene::LightingServiceComponent>(furnished.FindFirstRoot("Lighting"));
		Require(defaults != nullptr);
		for (size_t hour = 0; hour < 24; ++hour) {
			defaults->ClockTime = static_cast<float>(hour);
			Check(scene::LightingOf(furnished), defaults);
		}
		for (bool reversed : {false, true}) {
			ecs::Store store("sunlight.oracle.duplicates");
			const auto klass = ecs::Classes::Find(core::Name("Lighting"));
			const auto first = store.CreateInstance(klass, reversed ? "Second" : "First");
			const auto second = store.CreateInstance(klass, reversed ? "First" : "Second");
			auto *chosen = store.GetMutable<scene::LightingServiceComponent>(first);
			auto *ignored = store.GetMutable<scene::LightingServiceComponent>(second);
			Require(chosen != nullptr && ignored != nullptr);
			ignored->Brightness = 31.0f;
			ignored->Ambient = {0.0f, 0.0f, 0.0f};
			chosen->Ambient = {0.125f, 0.25f, 0.5f};
			chosen->OutdoorAmbient = {0.5f, 0.25f, 0.125f};
			chosen->Brightness = 2.0f;
			const auto addEffect = [&](const char *className) {
				const auto entity =
					store.CreateInstance(ecs::Classes::Find(core::Name(className)), className);
				Require(store.SetParent(entity, first));
				return entity;
			};
			auto *fog = store.GetMutable<scene::FogEffect>(addEffect("FogEffect"));
			auto *bloom = store.GetMutable<scene::BloomEffect>(addEffect("BloomEffect"));
			auto *depth = store.GetMutable<scene::DepthOfFieldEffect>(addEffect("DepthOfFieldEffect"));
			auto *rays = store.GetMutable<scene::GodRaysEffect>(addEffect("GodRaysEffect"));
			Require(fog && bloom && depth && rays);
			fog->Color = {0.75f, 0.5f, 0.25f};
			fog->Start = 8.0f;
			fog->End = 32.0f;
			bloom->Threshold = 0.5f;
			bloom->Intensity = 1.5f;
			bloom->Radius = 4.0f;
			depth->Intensity = 0.75f;
			depth->FocusDistance = 16.0f;
			depth->FocusRange = 2.0f;
			depth->Radius = 8.0f;
			rays->Intensity = 0.25f;
			rays->Threshold = 0.75f;
			rays->Radius = 64.0f;
			chosen->RenderFeatures = {3, 4};
			for (size_t hour = 0; hour < 24; ++hour) {
				chosen->ClockTime = static_cast<float>(hour) + 0.25f;
				chosen->GeographicLatitude = 30.0f;
				Check(scene::LightingOf(store), chosen, fog, bloom, depth, rays);
			}
			Require(store.SetInstanceName(first, "Renamed selected service"));
			chosen->ClockTime = -0.25f;
			chosen->GeographicLatitude = 128.0f;
			chosen->Brightness = -2.0f;
			fog->Start = -1.0f;
			fog->End = -4.0f;
			bloom->Threshold = -1.0f;
			bloom->Intensity = -2.0f;
			bloom->Radius = -3.0f;
			depth->Intensity = 2.0f;
			depth->FocusDistance = -4.0f;
			depth->FocusRange = -5.0f;
			depth->Radius = -6.0f;
			rays->Intensity = -7.0f;
			rays->Threshold = -8.0f;
			rays->Radius = -9.0f;
			chosen->RenderFeatures = {8, 3};
			Check(scene::LightingOf(store), chosen, fog, bloom, depth, rays);
			// Removing the selected service must expose the next authored instance.
			store.Destroy(first);
			Check(scene::LightingOf(store), store.Get<scene::LightingServiceComponent>(second));
		}
	}
}
