#include "WeatherMethods.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/scene/WeatherCompute.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/InstanceShim.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace engine::script {
	void SampleWeather(ScriptCall &call) {
		ENGINE_PROFILE_CAT("script weather sample", core::ProfileCategory::Script);
		auto &store = call.World();
		const auto provider =
			InstanceForScriptRead(store, call.Subject(), call.Role().Client && !call.Role().Server);
		if (!store.IsA(provider, ecs::Classes::Find(core::Name("WeatherCompute"))))
			call.Raise("Sample requires a WeatherCompute");
		if (call.Arguments() < 1 || call.Arguments() > 2)
			call.Raise("Sample requires a position and optional ground height");
		ecs::AttributeValue position;
		call.ReadAttribute(0, position);
		if (position.Type != ecs::PropertyType::Vector3 || !std::isfinite(position.Vector3.X) ||
			!std::isfinite(position.Vector3.Y) || !std::isfinite(position.Vector3.Z))
			call.Raise("Sample requires a finite Vector3 position in metres");
		const double ground = call.IsNil(1) ? 0.0 : call.AsNumber(1);
		if (!std::isfinite(ground) || std::abs(ground) > std::numeric_limits<float>::max())
			call.Raise("Sample requires a finite ground height in metres");
		const auto *controls = scene::WeatherControlsOf(store, provider);
		if (!store.Has<scene::WeatherCompute>(provider) || !controls || !controls->Enabled) {
			call.ReturnNil();
			return;
		}
		const auto sample =
			weather::QueryWeather(store, provider, position.Vector3, static_cast<float>(ground));
		if (!sample) {
			call.ReturnNil();
			return;
		}
		// Preparing the query can migrate ECS rows. Resolve controls again after that boundary.
		controls = scene::WeatherControlsOf(store, provider);
		uint64_t tick = 0, epoch = 0;
		if (controls->Mode == scene::WeatherMode::Stateful) {
			if (const auto *state = store.Get<weather::StatefulWeatherState>(provider)) {
				tick = state->Simulation.Tick;
				epoch = state->Simulation.Definition.WorldEpoch;
			} else if (const auto *checkpoint = store.Get<weather::StatefulWeatherCheckpoint>(provider)) {
				tick = checkpoint->Tick;
				epoch = checkpoint->Epoch;
			}
		} else if (const auto *clock = store.Get<scene::WeatherClock>(provider)) {
			tick = clock->Tick;
			epoch = clock->Epoch;
		}
		ScriptValue result{ValueTag::Map};
		result.Entries.reserve(19);
		const auto number = [&](const char *name, double value) {
			ScriptValue field{ValueTag::Number};
			field.Number = value;
			result.Entries.emplace_back(name, std::move(field));
		};
		const auto text = [&](const char *name, std::string value) {
			ScriptValue field{ValueTag::String};
			field.Text = std::move(value);
			result.Entries.emplace_back(name, std::move(field));
		};
		text("Layer", weather::WeatherLayerName(store, provider));
		text("Mode", scene::Describe(controls->Mode));
		// Both VMs use doubles. Decimal text preserves canonical uint64 metadata above 2^53.
		text("Tick", std::to_string(tick));
		text("Epoch", std::to_string(epoch));
		number("DefinitionRevision", controls->DefinitionRevision);
		number("CondensateKilogramsPerCubicMetre", sample->Air.CondensateKilogramsPerCubicMetre);
		number("VapourKilogramsPerKilogram", sample->Air.VapourKilogramsPerKilogram);
		number("TemperatureCelsius", sample->Air.TemperatureCelsius);
		number("PressureHectopascals", sample->Air.PressureHectopascals);
		ScriptValue wind{ValueTag::Vector3};
		wind.Vector = sample->Air.WindMetresPerSecond;
		result.Entries.emplace_back("WindMetresPerSecond", std::move(wind));
		number("IceFraction", sample->Air.IceFraction);
		number("LiquidMillimetresPerHour", sample->Precipitation.LiquidMillimetresPerHour);
		number("SnowMillimetresPerHour", sample->Precipitation.SnowMillimetresPerHour);
		number("HailMillimetresPerHour", sample->Precipitation.HailMillimetresPerHour);
		number("MeltingFraction", sample->Precipitation.MeltingFraction);
		number("SpawnSeed", sample->Precipitation.SpawnSeed);
		number("FogExtinctionPerMetre", sample->FogExtinctionPerMetre);
		number("ChargeSeparation", sample->ChargeSeparation);
		number("HailGrowth", sample->HailGrowth);
		call.ReturnValue(result);
	}
}
