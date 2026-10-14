#include "WeatherComputeRegistration.hpp"

#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/WeatherCompute.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string_view>
#include <type_traits>

namespace engine::scene {

	namespace {
		float Bounded(float value, float minimum, float maximum, float fallback) {
			return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
		}

		bool Finite(const WeatherCompute &weather) {
			const std::array scalars{
				weather.AtmosphereDensityStrength,
				weather.AtmosphereHazeStrength,
				weather.AtmosphereGlareSuppression,

				weather.CellSizeMetres,
				weather.BaseHeightMetres,
				weather.ThicknessMetres,
				weather.ExtentMetres.X,
				weather.ExtentMetres.Y,
				weather.ExtentMetres.Z,
				weather.Coverage,
				weather.CondensateScale,
				weather.VapourMixingRatio,
				weather.VapourScale,
				weather.BaseTemperatureCelsius,
				weather.BasePressureHectopascals,
				weather.TemperatureLapseCelsiusPerKilometre,
				weather.TemperatureVariationCelsius,
				weather.PressureVariationHectopascals,
				weather.WindMetresPerSecond.X,
				weather.WindMetresPerSecond.Y,
				weather.WindMetresPerSecond.Z,
				weather.TurbulenceMetresPerSecond,
				weather.ShearMetresPerSecond,
				weather.LiftMetresPerSecond,
				weather.FrontContrast,
				weather.GrowthRate,
				weather.TimeScale,
				weather.MaintenanceSeconds,
				weather.LodError,
				weather.ShadowStrength,
				weather.ShadowDistanceMetres,
				weather.ShadowUpdateSeconds,
				weather.FogStrength,
				weather.PrecipitationRate,
				weather.IceFraction,
				weather.StormIntensity,
				weather.HailIntensity,
				weather.LightningRate
			};
			return std::all_of(scalars.begin(), scalars.end(), [](float scalar) {
				return std::isfinite(scalar);
			});
		}

		core::Name WeatherModeEnum() {
			return core::Name("WeatherMode");
		}

		template <auto Member> ecs::PropertyDescriptor WeatherProperty(std::string_view name) {
			using Value = std::remove_cvref_t<decltype(WeatherCompute{}.*Member)>;
			ecs::PropertyDescriptor property;
			property.Name = core::Name(name);
			if constexpr (std::is_same_v<Value, float>) {
				property.Type = ecs::PropertyType::Float;
			} else if constexpr (std::is_same_v<Value, uint32_t>) {
				property.Type = ecs::PropertyType::Int32;
			} else if constexpr (std::is_same_v<Value, bool>) {
				property.Type = ecs::PropertyType::Bool;
			} else if constexpr (std::is_same_v<Value, core::Name>) {
				property.Type = ecs::PropertyType::Name;
			} else {
				static_assert(std::is_same_v<Value, core::Vector3>);
				property.Type = ecs::PropertyType::Vector3;
			}
			property.Size = sizeof(Value);
			property.Kind = ecs::PropertyKind::Field;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<WeatherCompute>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *output) {
				const auto *weather = store.Get<WeatherCompute>(instance);
				if (weather == nullptr) return false;
				*static_cast<Value *>(output) = weather->*Member;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *input) {
				const auto *weather = store.Get<WeatherCompute>(instance);
				if (weather == nullptr) return false;
				if constexpr (std::is_same_v<Value, uint32_t>) {
					if constexpr (Member != &WeatherCompute::Seed) {
						if (*static_cast<const int32_t *>(input) < 0) return false;
					}
				}
				if constexpr (std::is_same_v<Value, core::Name>) {
					const auto name = *static_cast<const core::Name *>(input);
					if (!name.IsValid() || name.Text().size() > MAX_WEATHER_LAYER_NAME_BYTES) return false;
				}
				WeatherCompute candidate = *weather;
				candidate.*Member = *static_cast<const Value *>(input);
				if (!Finite(candidate)) return false;
				candidate = ClampedWeatherCompute(candidate);
				if (!ValidWeatherCompute(candidate)) return false;
				if (candidate == *weather) return true;
				// Revision zero is reserved. Equality-based caches see wrap as a new definition.
				candidate.DefinitionRevision =
					weather->DefinitionRevision == UINT32_MAX ? 1 : weather->DefinitionRevision + 1;
				store.Set(instance, candidate);
				return true;
			};
			return property;
		}

		ecs::PropertyDescriptor ModeProperty() {
			auto property = WeatherProperty<&WeatherCompute::Enabled>("Mode");
			property.Type = ecs::PropertyType::Enum;
			property.EnumName = WeatherModeEnum();
			property.Size = sizeof(core::Name);
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *output) {
				const auto *weather = store.Get<WeatherCompute>(instance);
				if (weather == nullptr) return false;
				*static_cast<core::Name *>(output) = core::Name(Describe(weather->Mode));
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *input) {
				const auto *weather = store.Get<WeatherCompute>(instance);
				if (weather == nullptr) return false;
				WeatherMode mode;
				if (!WeatherModeFromName(*static_cast<const core::Name *>(input), mode)) return false;
				if (mode == weather->Mode) return true;
				auto candidate = *weather;
				candidate.Mode = mode;
				if (!ValidWeatherCompute(candidate)) return false;
				candidate.DefinitionRevision =
					weather->DefinitionRevision == UINT32_MAX ? 1 : weather->DefinitionRevision + 1;
				store.Set(instance, candidate);
				return true;
			};
			return property;
		}
	}

	const WeatherCompute *WeatherControlsOf(const ecs::Store &store, ecs::Entity provider) {
		if (const auto *accepted = store.Get<WeatherAcceptedDefinition>(provider)) return &accepted->Controls;
		return store.Get<WeatherCompute>(provider);
	}

	const char *Describe(WeatherMode mode) {
		switch (mode) {
		case WeatherMode::Procedural:
			return "Procedural";
		case WeatherMode::Stateful:
			return "Stateful";
		}
		return "Procedural";
	}

	bool WeatherModeFromName(core::Name name, WeatherMode &mode) {
		if (name == core::Name("Procedural")) {
			mode = WeatherMode::Procedural;
			return true;
		}
		if (name == core::Name("Stateful")) {
			mode = WeatherMode::Stateful;
			return true;
		}
		return false;
	}

	WeatherCompute ClampedWeatherCompute(WeatherCompute weather) {
		const WeatherCompute defaults;
		weather.CellSizeMetres = Bounded(weather.CellSizeMetres, 1.0f, 4096.0f, defaults.CellSizeMetres);
		weather.BaseHeightMetres =
			Bounded(weather.BaseHeightMetres, -16384.0f, 65536.0f, defaults.BaseHeightMetres);
		weather.ThicknessMetres = Bounded(weather.ThicknessMetres, 1.0f, 65536.0f, defaults.ThicknessMetres);
		weather.Coverage = Bounded(weather.Coverage, 0.0f, 1.0f, defaults.Coverage);
		weather.CondensateScale = Bounded(weather.CondensateScale, 1e-06f, 1.0f, defaults.CondensateScale);
		weather.VapourMixingRatio =
			Bounded(weather.VapourMixingRatio, 0.0f, 0.1f, defaults.VapourMixingRatio);
		weather.VapourScale = Bounded(weather.VapourScale, 1e-06f, 0.1f, defaults.VapourScale);
		weather.BaseTemperatureCelsius =
			Bounded(weather.BaseTemperatureCelsius, -100.0f, 100.0f, defaults.BaseTemperatureCelsius);
		weather.BasePressureHectopascals =
			Bounded(weather.BasePressureHectopascals, 1.0f, 1200.0f, defaults.BasePressureHectopascals);
		weather.TemperatureLapseCelsiusPerKilometre = Bounded(
			weather.TemperatureLapseCelsiusPerKilometre,
			-20.0f,
			20.0f,
			defaults.TemperatureLapseCelsiusPerKilometre
		);
		weather.TemperatureVariationCelsius =
			Bounded(weather.TemperatureVariationCelsius, 0.0f, 32.0f, defaults.TemperatureVariationCelsius);
		weather.PressureVariationHectopascals = Bounded(
			weather.PressureVariationHectopascals, 0.0f, 32.0f, defaults.PressureVariationHectopascals
		);
		weather.TurbulenceMetresPerSecond =
			Bounded(weather.TurbulenceMetresPerSecond, 0.0f, 16.0f, defaults.TurbulenceMetresPerSecond);
		weather.ShearMetresPerSecond =
			Bounded(weather.ShearMetresPerSecond, -32.0f, 32.0f, defaults.ShearMetresPerSecond);
		weather.LiftMetresPerSecond =
			Bounded(weather.LiftMetresPerSecond, -16.0f, 16.0f, defaults.LiftMetresPerSecond);
		weather.FrontContrast = Bounded(weather.FrontContrast, 0.0f, 1.0f, defaults.FrontContrast);
		weather.GrowthRate = Bounded(weather.GrowthRate, 0.0f, 1.0f, defaults.GrowthRate);
		weather.TimeScale = Bounded(weather.TimeScale, 0.0f, 16.0f, defaults.TimeScale);
		weather.MaintenanceSeconds =
			Bounded(weather.MaintenanceSeconds, 0.05f, 30.0f, defaults.MaintenanceSeconds);
		weather.LodError = Bounded(weather.LodError, 0.0001f, 1.0f, defaults.LodError);
		weather.ShadowStrength = Bounded(weather.ShadowStrength, 0.0f, 1.0f, defaults.ShadowStrength);
		weather.ShadowDistanceMetres =
			Bounded(weather.ShadowDistanceMetres, 0.0f, 1048576.0f, defaults.ShadowDistanceMetres);
		weather.ShadowUpdateSeconds =
			Bounded(weather.ShadowUpdateSeconds, 0.01f, 30.0f, defaults.ShadowUpdateSeconds);
		weather.AtmosphereDensityStrength =
			Bounded(weather.AtmosphereDensityStrength, 0.0f, 1.0f, defaults.AtmosphereDensityStrength);
		weather.AtmosphereHazeStrength =
			Bounded(weather.AtmosphereHazeStrength, 0.0f, 1.0f, defaults.AtmosphereHazeStrength);
		weather.AtmosphereGlareSuppression =
			Bounded(weather.AtmosphereGlareSuppression, 0.0f, 1.0f, defaults.AtmosphereGlareSuppression);
		weather.FogStrength = Bounded(weather.FogStrength, 0.0f, 1.0f, defaults.FogStrength);
		weather.PrecipitationRate =
			Bounded(weather.PrecipitationRate, 0.0f, 1000.0f, defaults.PrecipitationRate);
		weather.IceFraction = Bounded(weather.IceFraction, 0.0f, 1.0f, defaults.IceFraction);
		weather.StormIntensity = Bounded(weather.StormIntensity, 0.0f, 1.0f, defaults.StormIntensity);
		weather.HailIntensity = Bounded(weather.HailIntensity, 0.0f, 1.0f, defaults.HailIntensity);
		weather.LightningRate = Bounded(weather.LightningRate, 0.0f, 10.0f, defaults.LightningRate);
		weather.ExtentMetres.X = Bounded(weather.ExtentMetres.X, 1.0f, 1048576.0f, defaults.ExtentMetres.X);
		weather.ExtentMetres.Y = Bounded(weather.ExtentMetres.Y, 1.0f, 1048576.0f, defaults.ExtentMetres.Y);
		weather.ExtentMetres.Z = Bounded(weather.ExtentMetres.Z, 1.0f, 1048576.0f, defaults.ExtentMetres.Z);
		weather.WindMetresPerSecond.X =
			Bounded(weather.WindMetresPerSecond.X, -64.0f, 64.0f, defaults.WindMetresPerSecond.X);
		weather.WindMetresPerSecond.Y =
			Bounded(weather.WindMetresPerSecond.Y, -16.0f, 16.0f, defaults.WindMetresPerSecond.Y);
		weather.WindMetresPerSecond.Z =
			Bounded(weather.WindMetresPerSecond.Z, -64.0f, 64.0f, defaults.WindMetresPerSecond.Z);
		weather.VapourMixingRatio = std::min(weather.VapourMixingRatio, weather.VapourScale);
		weather.FeatureCells = std::bit_floor(std::clamp(weather.FeatureCells, 1u, 1024u));
		weather.TickRate = std::clamp(weather.TickRate, 1u, 120u);
		weather.MaximumResidentBricks = std::clamp(weather.MaximumResidentBricks, 1u, 65536u);
		weather.MaximumLod = std::clamp(weather.MaximumLod, 0u, 8u);
		weather.ShadowCascades = std::clamp(weather.ShadowCascades, 1u, 4u);
		weather.ShadowResolution = std::bit_floor(std::clamp(weather.ShadowResolution, 32u, 2048u));
		weather.DefinitionRevision = std::max(weather.DefinitionRevision, 1u);
		if (!weather.LayerName.IsValid() || weather.LayerName.Text().size() > MAX_WEATHER_LAYER_NAME_BYTES)
			weather.LayerName = defaults.LayerName;
		return weather;
	}

	bool ValidWeatherCompute(const WeatherCompute &weather) {
		return Finite(weather) && weather.AlgorithmVersion == 1 &&
			   (weather.Mode == WeatherMode::Procedural || weather.Mode == WeatherMode::Stateful) &&
			   ClampedWeatherCompute(weather) == weather;
	}

	void RegisterWeatherComputeProperties(ecs::ClassId owner) {
		ecs::EnumTable::Register(
			WeatherModeEnum().Text(), std::array<std::string_view, 2>{"Procedural", "Stateful"}
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::CellSizeMetres>("CellSizeMetres"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::BaseHeightMetres>("BaseHeightMetres"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::ThicknessMetres>("ThicknessMetres"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::ExtentMetres>("ExtentMetres"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::Seed>("Seed"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::Coverage>("Coverage"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::CondensateScale>("CondensateScale"));
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::VapourMixingRatio>("VapourMixingRatio")
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::VapourScale>("VapourScale"));
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::BaseTemperatureCelsius>("BaseTemperatureCelsius")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::BasePressureHectopascals>("BasePressureHectopascals")
		);
		ecs::Classes::Computed(
			owner,
			WeatherProperty<&WeatherCompute::TemperatureLapseCelsiusPerKilometre>(
				"TemperatureLapseCelsiusPerKilometre"
			)
		);
		ecs::Classes::Computed(
			owner,
			WeatherProperty<&WeatherCompute::TemperatureVariationCelsius>("TemperatureVariationCelsius")
		);
		ecs::Classes::Computed(
			owner,
			WeatherProperty<&WeatherCompute::PressureVariationHectopascals>("PressureVariationHectopascals")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::TurbulenceMetresPerSecond>("TurbulenceMetresPerSecond")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::ShearMetresPerSecond>("ShearMetresPerSecond")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::LiftMetresPerSecond>("LiftMetresPerSecond")
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::FrontContrast>("FrontContrast"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::GrowthRate>("GrowthRate"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::TimeScale>("TimeScale"));
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::MaintenanceSeconds>("MaintenanceSeconds")
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::LodError>("LodError"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::ShadowStrength>("ShadowStrength"));
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::ShadowDistanceMetres>("ShadowDistanceMetres")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::ShadowUpdateSeconds>("ShadowUpdateSeconds")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::AtmosphereDensityStrength>("AtmosphereDensityStrength")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::AtmosphereHazeStrength>("AtmosphereHazeStrength")
		);
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::AtmosphereGlareSuppression>("AtmosphereGlareSuppression")
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::FogStrength>("FogStrength"));
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::PrecipitationRate>("PrecipitationRate")
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::IceFraction>("IceFraction"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::StormIntensity>("StormIntensity"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::HailIntensity>("HailIntensity"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::LightningRate>("LightningRate"));
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::WindMetresPerSecond>("WindMetresPerSecond")
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::FeatureCells>("FeatureCells"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::TickRate>("TickRate"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::Paused>("Paused"));
		ecs::Classes::Computed(
			owner, WeatherProperty<&WeatherCompute::MaximumResidentBricks>("MaximumResidentBricks")
		);
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::MaximumLod>("MaximumLod"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::ShadowsEnabled>("ShadowsEnabled"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::ShadowResolution>("ShadowResolution"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::ShadowCascades>("ShadowCascades"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::Enabled>("WeatherEnabled"));
		ecs::Classes::Computed(owner, WeatherProperty<&WeatherCompute::LayerName>("LayerName"));
		ecs::Classes::Computed(owner, ModeProperty());
		auto algorithmVersion = WeatherProperty<&WeatherCompute::AlgorithmVersion>("AlgorithmVersion");
		algorithmVersion.Set = nullptr;
		algorithmVersion.Writable = false;
		algorithmVersion.Writes = &ecs::ComponentSet::Intern({});
		ecs::Classes::Computed(owner, algorithmVersion);
		auto definitionRevision = WeatherProperty<&WeatherCompute::DefinitionRevision>("DefinitionRevision");
		definitionRevision.Set = nullptr;
		definitionRevision.Writable = false;
		definitionRevision.Writes = &ecs::ComponentSet::Intern({});
		ecs::Classes::Computed(owner, definitionRevision);
	}
}
