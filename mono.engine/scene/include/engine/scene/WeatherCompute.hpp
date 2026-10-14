#pragma once

// Authored weather semantics. Device formats and resident cell storage belong to render.
// The WeatherCompute scene class derives CloudCompute; ECS stores these controls separately.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <string>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Bounds layer text before interning a name received from a file or peer.
	inline constexpr uint32_t MAX_WEATHER_LAYER_NAME_BYTES = 128;
	// Canonical provider path bound, shared with copied weather stream identities.
	inline constexpr uint32_t MAX_WEATHER_STREAM_LAYER_BYTES = 256;

	// Whether the layer is directly reconstructed or restored from solver checkpoints.
	enum class WeatherMode : uint8_t {
		// Reconstructs canonical samples directly at any accepted tick.
		Procedural,
		// Evolves complete solver state restored from a compatible checkpoint. Accepted
		// structural physics, seed and layer-purpose edits start a new physical epoch
		// at the current accepted weather phase. Wind, visual-only and provider-path
		// edits preserve the evolved cells.
		Stateful,
	};

	// World-unit weather controls carried alongside inherited Clouds appearance.
	struct WeatherCompute {
		// Canonical lattice geometry in metres. Extent is centred on the layer origin.
		//@{
		float CellSizeMetres = 64.0f;
		float BaseHeightMetres = 1000.0f;
		float ThicknessMetres = 1600.0f;
		core::Vector3 ExtentMetres{16384.0f, 4096.0f, 16384.0f};
		//@}
		// Seed and algorithm revision describe a field independently of camera quality.
		//@{
		uint32_t Seed = 1;
		uint32_t AlgorithmVersion = 1;
		uint32_t DefinitionRevision = 1;
		WeatherMode Mode = WeatherMode::Procedural;
		//@}
		// Water and thermodynamic reference scales, in kg/m^3, kg/kg, Celsius and hPa.
		//@{
		float Coverage = 0.5f;
		float CondensateScale = 0.003f;
		float VapourMixingRatio = 0.012f;
		float VapourScale = 0.03f;
		float BaseTemperatureCelsius = 15.0f;
		float BasePressureHectopascals = 1013.25f;
		float TemperatureLapseCelsiusPerKilometre = 6.5f;
		float TemperatureVariationCelsius = 8.0f;
		float PressureVariationHectopascals = 12.0f;
		//@}
		// Velocity and scale controls in metres/second and canonical cells.
		//@{
		core::Vector3 WindMetresPerSecond{6.0f, 0.0f, 0.0f};
		float TurbulenceMetresPerSecond = 3.0f;
		float ShearMetresPerSecond = 4.0f;
		float LiftMetresPerSecond = 2.0f;
		uint32_t FeatureCells = 32;
		float FrontContrast = 0.3f;
		float GrowthRate = 0.25f;
		//@}
		// Accepted timeline rate. Maintenance cadence never changes the simulation rate.
		//@{
		uint32_t TickRate = 20;
		float TimeScale = 1.0f;
		bool Paused = false;
		float MaintenanceSeconds = 5.0f;
		//@}
		// Rendering quality and separately bounded cloud shadows.
		//@{
		uint32_t MaximumResidentBricks = 512;
		uint32_t MaximumLod = 4;
		float LodError = 0.04f;
		bool ShadowsEnabled = true;
		float ShadowStrength = 0.7f;
		float ShadowDistanceMetres = 8192.0f;
		uint32_t ShadowResolution = 256;
		uint32_t ShadowCascades = 1;
		float ShadowUpdateSeconds = 0.2f;
		//@}
		// Optional consumers share the canonical moisture and temperature field.
		//@{
		// Response strength in 0..1. Zero preserves each authored atmosphere property.
		//@{
		float AtmosphereDensityStrength = 0.0f;
		float AtmosphereHazeStrength = 0.0f;
		float AtmosphereGlareSuppression = 0.0f;
		//@}
		float FogStrength = 0.0f;
		// Water-equivalent precipitation in millimetres/hour, bounded to 0..1000.
		float PrecipitationRate = 0.0f;
		// Frozen condensate fraction, bounded to 0..1.
		float IceFraction = 0.0f;
		// Normalized storm forcing, bounded to 0..1.
		float StormIntensity = 0.0f;
		// Normalized growth, up to one impact/second at full cold retention.
		float HailIntensity = 0.0f;
		// Maximum discharges/second at full charge, bounded to 0..10.
		float LightningRate = 0.0f;
		// Stable nonempty layer purpose, stored as text and bounded to 128 bytes.
		core::Name LayerName{"default"};
		bool Enabled = true;
		//@}
		// Compares semantic values without depending on object padding.
		bool operator==(const WeatherCompute &) const = default;
	};

	// Immutable canonical controls for unfinished weather ticks. Authored WeatherCompute
	// remains the next desired definition, so pending edits cannot rewrite the backlog.
	struct WeatherAcceptedDefinition {
		// The single accepted definition shared by clocks, queries and presentation.
		WeatherCompute Controls;
		// Accepted full provider path; empty permits initial/manual fixtures to resolve their current path.
		std::string Layer = {};
	};

	// Accepted controls when present, otherwise authored controls, or null without a provider.
	// The borrowed pointer follows Store component lifetime and must not survive a mutation.
	[[nodiscard]] const WeatherCompute *WeatherControlsOf(const ecs::Store &store, ecs::Entity provider);

	// Stable mode name used in authored files and reflection.
	const char *Describe(WeatherMode mode);
	// Refuses unknown names without changing the destination mode.
	[[nodiscard]] bool WeatherModeFromName(core::Name name, WeatherMode &mode);
	// Refuses nonfinite, unsupported or out-of-range controls at serialized boundaries.
	[[nodiscard]] bool ValidWeatherCompute(const WeatherCompute &weather);
	// Sanitizes raw ECS writes for presentation without mutating authored storage.
	WeatherCompute ClampedWeatherCompute(WeatherCompute weather);
}
