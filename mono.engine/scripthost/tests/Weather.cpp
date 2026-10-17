#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/scene/WeatherCompute.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.scripthost.weather")
TEST_DEPENDS("engine.weather.runtime")
TEST_DEPENDS("engine.weather.statefulruntime")

using namespace engine;

namespace {
	void Register() {
		scene::RegisterSceneClasses();
		weather::RegisterWeatherRuntimeComponents();
		weather::RegisterStatefulWeatherComponents();
	}
	scene::WeatherCompute Controls() {
		scene::WeatherCompute controls;
		controls.CellSizeMetres = 64;
		controls.BaseHeightMetres = 0;
		controls.ThicknessMetres = 256;
		controls.ExtentMetres = {256, 256, 256};
		controls.TurbulenceMetresPerSecond = 0;
		controls.ShearMetresPerSecond = 0;
		controls.LiftMetresPerSecond = 0;
		controls.WindMetresPerSecond = {4, 0, 0};
		controls.PrecipitationRate = 20;
		controls.FogStrength = 0.5f;
		return controls;
	}
	ecs::Entity Provider(ecs::Store &store, const scene::WeatherCompute &controls) {
		scene::InstallServices(store);
		const auto lighting = scene::ServiceOf(store, ecs::Classes::Find(core::Name("Lighting")));
		const auto provider =
			store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Weather");
		REQUIRE(provider != ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(provider, lighting));
		store.Set(provider, controls);
		return provider;
	}
	void Attribute(ecs::Store &store, std::string_view key, const ecs::AttributeValue &value) {
		const auto workspace = scene::ServiceOf(store, ecs::Classes::Find(core::Name("Workspace")));
		REQUIRE(ecs::SetAttribute(store, workspace, core::Name(key), value));
	}
	void Expected(
		ecs::Store &store,
		ecs::Entity provider,
		uint64_t tick,
		uint64_t epoch,
		float ground = 0,
		std::string_view prefix = "Expected"
	) {
		const auto sample = weather::QueryWeather(store, provider, {0, 64, 0}, ground);
		REQUIRE(sample);
		const auto number = [&](const char *name, double value) {
			ecs::AttributeValue attribute;
			attribute.Type = ecs::PropertyType::Double;
			attribute.Double = value;
			Attribute(store, std::string(prefix) + name, attribute);
		};
		const auto text = [&](const char *name, std::string value) {
			ecs::AttributeValue attribute;
			attribute.Type = ecs::PropertyType::String;
			attribute.String = std::move(value);
			Attribute(store, std::string(prefix) + name, attribute);
		};
		const auto *controls = scene::WeatherControlsOf(store, provider);
		text("Layer", weather::WeatherLayerName(store, provider));
		text("Mode", scene::Describe(controls->Mode));
		text("Tick", std::to_string(tick));
		text("Epoch", std::to_string(epoch));
		number("DefinitionRevision", controls->DefinitionRevision);
		number("CondensateKilogramsPerCubicMetre", sample->Air.CondensateKilogramsPerCubicMetre);
		number("VapourKilogramsPerKilogram", sample->Air.VapourKilogramsPerKilogram);
		number("TemperatureCelsius", sample->Air.TemperatureCelsius);
		number("PressureHectopascals", sample->Air.PressureHectopascals);
		ecs::AttributeValue wind;
		wind.Type = ecs::PropertyType::Vector3;
		wind.Vector3 = sample->Air.WindMetresPerSecond;
		Attribute(store, std::string(prefix) + "WindMetresPerSecond", wind);
		number("IceFraction", sample->Air.IceFraction);
		number("LiquidMillimetresPerHour", sample->Precipitation.LiquidMillimetresPerHour);
		number("SnowMillimetresPerHour", sample->Precipitation.SnowMillimetresPerHour);
		number("HailMillimetresPerHour", sample->Precipitation.HailMillimetresPerHour);
		number("MeltingFraction", sample->Precipitation.MeltingFraction);
		number("SpawnSeed", sample->Precipitation.SpawnSeed);
		number("FogExtinctionPerMetre", sample->FogExtinctionPerMetre);
		number("ChargeSeparation", sample->ChargeSeparation);
		number("HailGrowth", sample->HailGrowth);
	}
	std::string Prefix(script::Language language) {
		return language == script::Language::Luau
				   ? "local w = game:GetService('Lighting'):FindFirstChild('Weather')\n"
					 "local p = Vector3.new(0, 64, 0)\n"
				   : "const w = game.GetService('Lighting').FindFirstChild('Weather');\n"
					 "const p = Vector3.new(0, 64, 0);\n";
	}
	std::string Compare(script::Language language, std::string_view expression, std::string_view prefix) {
		std::string source = language == script::Language::Luau ? "local s = " : "let s = ";
		source += expression;
		source += language == script::Language::Luau ? "\nassert(s ~= nil)\n"
													 : ";\nif (!s) throw new Error('missing sample');\n";
		if (language == script::Language::Luau) {
			source += "local count = 0\nfor k, v in pairs(s) do\n count += 1\n";
			source += " local expected = workspace:GetAttribute('" + std::string(prefix) + "' .. k)\n";
			source += " assert(v == expected, k)\nend\nassert(count == 19)\n";
		} else {
			source += "if (Object.keys(s).length !== 19) throw new Error('sample shape');\nfor (const k of "
					  "Object.keys(s)) {\n";
			source += " const expected = workspace.GetAttribute('" + std::string(prefix) + "' + k);\n";
			source += " if (k === 'WindMetresPerSecond' ? !s[k].Equals(expected) : s[k] !== expected) throw "
					  "new Error(k);\n}\n";
		}
		return source;
	}
	void Run(script::Runtime &runtime, script::Language language, const std::string &source) {
		const std::string wrapped =
			language == script::Language::Luau ? "do\n" + source + "\nend" : "{\n" + source + "\n}";
		const bool ran = runtime.Run(wrapped, "weather-sample");
		INFO(runtime.LastError());
		REQUIRE(ran);
	}
}

TEST_CASE(
	"weather queries preserve canonical metadata and copied physical units in both VMs", "[script][weather]"
) {
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		Register();
		ecs::Store store("script.weather.procedural");
		const auto controls = Controls();
		const auto provider = Provider(store, controls);
		scene::WeatherClock clock;
		clock.Tick = (uint64_t{1} << 54) + 3;
		clock.AnchorTick = clock.Tick;
		clock.PhaseTick = 100;
		clock.Epoch = (uint64_t{1} << 54) + 7;
		store.Set(provider, clock);
		Expected(store, provider, clock.Tick, clock.Epoch);
		Expected(store, provider, clock.Tick, clock.Epoch, 32, "Ground");
		// A receipt estimate is for presentation only. Scripts read the accepted integral model.
		scene::WeatherClockEstimate estimate;
		estimate.Tick = clock.Tick + 10;
		store.Set(provider, estimate);
		const auto runtime = script::MakeRuntime(store, language);
		std::string source =
			Prefix(language) +
			Compare(language, language == script::Language::Luau ? "w:Sample(p)" : "w.Sample(p)", "Expected");
		source += language == script::Language::Luau
					  ? "s.TemperatureCelsius = 999\ns.WindMetresPerSecond = Vector3.zero\n"
					  : "s.TemperatureCelsius = 999; s.WindMetresPerSecond = Vector3.zero;\n";
		// Re-enter a nested scope so the comparison's dictionary variable can be reused in JavaScript.
		source += language == script::Language::Luau ? "do\n" : "{\n";
		source += Compare(
			language,
			language == script::Language::Luau ? "w:Sample(p, nil)" : "w.Sample(p, null)",
			"Expected"
		);
		source += language == script::Language::Luau ? "end\ndo\n" : "}\n{\n";
		source += Compare(
			language, language == script::Language::Luau ? "w:Sample(p, 32)" : "w.Sample(p, 32)", "Ground"
		);
		source += language == script::Language::Luau ? "end\n" : "}\n";
		Run(*runtime, language, source);
		CHECK(*store.Get<scene::WeatherCompute>(provider) == controls);
		CHECK(store.Get<scene::WeatherClock>(provider)->Tick == clock.Tick);
		// Raw desired edits cannot overwrite the accepted model while its ticks remain pending.
		auto desired = controls;
		desired.BaseTemperatureCelsius += 5;
		++desired.DefinitionRevision;
		store.Set(provider, desired);
		Run(*runtime,
			language,
			Prefix(language) +
				Compare(
					language, language == script::Language::Luau ? "w:Sample(p)" : "w.Sample(p)", "Expected"
				));
		store.Set(
			provider, scene::WeatherAcceptedDefinition{desired, weather::WeatherLayerName(store, provider)}
		);
		clock.DefinitionRevision = desired.DefinitionRevision;
		store.Set(provider, clock);
		Expected(store, provider, clock.Tick, clock.Epoch);
		Run(*runtime,
			language,
			Prefix(language) +
				Compare(
					language, language == script::Language::Luau ? "w:Sample(p)" : "w.Sample(p)", "Expected"
				));
	}
}

TEST_CASE(
	"weather script queries refuse malformed input and retired or disabled providers", "[script][weather]"
) {
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		Register();
		ecs::Store store("script.weather.refusal");
		const auto provider = Provider(store, Controls());
		const auto runtime = script::MakeRuntime(store, language);
		// Authoring a provider does not fabricate a canonical clock before its first simulation tick.
		REQUIRE_FALSE(store.Has<scene::WeatherClock>(provider));
		REQUIRE_FALSE(weather::QueryWeather(store, provider, {0, 64, 0}));
		Run(*runtime,
			language,
			Prefix(language) +
				(language == script::Language::Luau
					 ? "assert(w:Sample(p) == nil)"
					 : "if (w.Sample(p) !== null) throw new Error('before-first-clock query');"));
		store.Set(provider, scene::WeatherClock{});
		Run(*runtime,
			language,
			Prefix(language) + (language == script::Language::Luau ? R"(
local function refused(f) assert(not pcall(f)) end
refused(function() w:Sample() end)
refused(function() w:Sample('bad') end)
refused(function() w:Sample(Vector3.new(0/0, 0, 0)) end)
refused(function() w:Sample(p, 'bad') end)
refused(function() w:Sample(p, math.huge) end)
refused(function() w:Sample(p, 0, 1) end)
refused(function() workspace:Sample(p) end)
assert(w:Sample(p))
)"
																   : R"(
function refused(f) { let caught = false; try { f(); } catch (_) { caught = true; } if (!caught) throw new Error('input accepted'); }
refused(() => w.Sample());
refused(() => w.Sample('bad'));
refused(() => w.Sample(Vector3.new(NaN, 0, 0)));
refused(() => w.Sample(p, 'bad'));
refused(() => w.Sample(p, Infinity));
refused(() => w.Sample(p, 0, 1));
refused(() => workspace.Sample(p));
if (!w.Sample(p)) throw new Error('valid query refused');
)"));
		auto disabled = *scene::WeatherControlsOf(store, provider);
		disabled.Enabled = false;
		store.Set(
			provider, scene::WeatherAcceptedDefinition{disabled, weather::WeatherLayerName(store, provider)}
		);
		Run(*runtime,
			language,
			Prefix(language) + (language == script::Language::Luau
									? "assert(w:Sample(p) == nil)"
									: "if (w.Sample(p) !== null) throw new Error('disabled query');"));
		disabled.Enabled = true;
		store.Set(
			provider, scene::WeatherAcceptedDefinition{disabled, weather::WeatherLayerName(store, provider)}
		);
		store.Remove<scene::WeatherClock>(provider);
		Run(*runtime,
			language,
			Prefix(language) + (language == script::Language::Luau
									? "assert(w:Sample(p) == nil)"
									: "if (w.Sample(p) !== null) throw new Error('missing clock query');"));
		store.Remove<scene::WeatherCompute>(provider);
		Run(*runtime,
			language,
			Prefix(language) + (language == script::Language::Luau
									? "assert(w:Sample(p) == nil)"
									: "if (w.Sample(p) !== null) throw new Error('retired query');"));
		Run(*runtime,
			language,
			Prefix(language) +
				(language == script::Language::Luau
					 ? "w:Destroy()\nassert(not pcall(function() w:Sample(p) end))"
					 : "w.Destroy(); let refused = false; try { w.Sample(p); } catch (_) { refused = true; } "
					   "if (!refused) throw new Error('destroyed query');"));
	}
}

TEST_CASE(
	"replica weather scripts sample validated checkpoint bytes without a CPU grid",
	"[script][weather][replica]"
) {
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		Register();
		ecs::Store authority("script.weather.authority");
		auto controls = Controls();
		controls.Mode = scene::WeatherMode::Stateful;
		const auto provider = Provider(authority, controls);
		authority.AdvanceTick(0.2f);
		weather::UpdateWeatherClocks(authority);
		weather::UpdateStatefulWeather(authority);
		weather::UpdateWeatherRuntime(authority);
		REQUIRE(weather::PublishStatefulCheckpoint(authority, provider));
		const auto checkpoint = *authority.Get<weather::StatefulWeatherCheckpoint>(provider);
		ecs::Store replica("script.weather.replica");
		const auto replicaProvider = Provider(replica, controls);
		replica.Set(replicaProvider, *authority.Get<scene::WeatherAcceptedDefinition>(provider));
		replica.Set(replicaProvider, *authority.Get<scene::WeatherClock>(provider));
		replica.Set(replicaProvider, checkpoint);
		Expected(replica, replicaProvider, checkpoint.Tick, checkpoint.Epoch);
		replica.SetAdoptOnly(true);
		script::RuntimeLimits limits;
		limits.Role = script::HostRole::OfClient();
		const auto runtime = script::MakeRuntime(replica, language, limits);
		Run(*runtime,
			language,
			Prefix(language) +
				Compare(
					language, language == script::Language::Luau ? "w:Sample(p)" : "w.Sample(p)", "Expected"
				));
		CHECK_FALSE(replica.Has<weather::StatefulWeatherState>(replicaProvider));
		CHECK(replica.Get<weather::StatefulWeatherCheckpoint>(replicaProvider)->Bytes == checkpoint.Bytes);
		// A forged payload cannot be promoted to a script-visible physical atmosphere.
		auto malformed = checkpoint;
		malformed.Bytes.front() ^= std::byte{1};
		replica.Set(replicaProvider, malformed);
		REQUIRE_FALSE(weather::QueryWeather(replica, replicaProvider, {0, 64, 0}));
		Run(*runtime,
			language,
			Prefix(language) +
				(language == script::Language::Luau
					 ? "assert(w:Sample(p) == nil)"
					 : "if (w.Sample(p) !== null) throw new Error('malformed checkpoint query');"));
	}
}
