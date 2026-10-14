#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/WeatherCompute.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.scene.weathercompute")

using namespace engine;

namespace {
	ecs::Entity Weather(ecs::Store &store) {
		scene::RegisterSceneClasses();
		return store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Weather");
	}

	ecs::Entity Lighting(ecs::Store &store) {
		scene::InstallServices(store);
		return scene::ServiceOf(store, ecs::Classes::Find(core::Name("Lighting")));
	}
}

TEST_CASE("weather is one complete authored cloud provider", "[scene][weather]") {
	ecs::Store store("weather.class");
	const auto provider = Weather(store);
	const auto klass = store.ClassOf(provider);
	CHECK(ecs::Classes::Describe(klass).Parent == ecs::Classes::Find(core::Name("CloudCompute")));
	CHECK(store.Has<scene::Clouds>(provider));
	CHECK(store.Has<scene::CloudCompute>(provider));
	REQUIRE(store.Get<scene::WeatherCompute>(provider) != nullptr);
	CHECK(scene::ValidWeatherCompute(*store.Get<scene::WeatherCompute>(provider)));
	CHECK_FALSE(store.Has<scene::Transform>(provider));
	CHECK_FALSE(scene::EnvironmentOf(store).HasWeatherCompute);
	REQUIRE(store.SetParent(provider, Lighting(store)));
	const auto environment = scene::EnvironmentOf(store);
	CHECK(environment.HasClouds);
	CHECK(environment.HasCloudCompute);
	CHECK(environment.HasWeatherCompute);
	CHECK(environment.CloudProvider == provider);
	CHECK(environment.Weather == *store.Get<scene::WeatherCompute>(provider));
	core::Name mode;
	REQUIRE(store.GetProperty(provider, core::Name("Mode"), &mode, sizeof(mode)));
	CHECK(mode == core::Name("Procedural"));
	for (const auto &property : store.PropertiesOf(provider)) {
		if (property.Name == core::Name("Mode")) {
			CHECK(property.Type == ecs::PropertyType::Enum);
			CHECK(property.EnumName == core::Name("WeatherMode"));
		}
	}
}

TEST_CASE("weather controls refuse nonfinite writes and publish bounded changes once", "[scene][weather]") {
	ecs::Store store("weather.properties");
	const auto provider = Weather(store);
	store.Observe<scene::WeatherCompute>();
	const float coverage = 4.0f;
	REQUIRE(store.SetProperty(provider, core::Name("Coverage"), &coverage, sizeof(coverage)));
	CHECK(store.Get<scene::WeatherCompute>(provider)->Coverage == 1.0f);
	const auto revision = store.Get<scene::WeatherCompute>(provider)->DefinitionRevision;
	store.ClearChanges();
	REQUIRE(store.SetProperty(provider, core::Name("Coverage"), &coverage, sizeof(coverage)));
	CHECK_FALSE(store.Changed<scene::WeatherCompute>(provider));
	CHECK(store.Get<scene::WeatherCompute>(provider)->DefinitionRevision == revision);
	for (const auto &property : store.PropertiesOf(provider)) {
		if (property.Type != ecs::PropertyType::Float || property.Reads == nullptr ||
			!property.Reads->Contains(ecs::Components::Of<scene::WeatherCompute>()))
			continue;
		const auto before = *store.Get<scene::WeatherCompute>(provider);
		for (const float invalid :
			 {std::numeric_limits<float>::quiet_NaN(),
			  std::numeric_limits<float>::infinity(),
			  -std::numeric_limits<float>::infinity()}) {
			CHECK_FALSE(store.SetProperty(provider, property.Name, &invalid, sizeof(invalid)));
			CHECK(*store.Get<scene::WeatherCompute>(provider) == before);
		}
	}
	const core::Vector3 wind{100, -100, 0};
	REQUIRE(store.SetProperty(provider, core::Name("WindMetresPerSecond"), &wind, sizeof(wind)));
	CHECK(store.Get<scene::WeatherCompute>(provider)->WindMetresPerSecond == core::Vector3{64, -16, 0});
	const core::Vector3 invalidExtent{1, std::numeric_limits<float>::quiet_NaN(), 1};
	CHECK_FALSE(
		store.SetProperty(provider, core::Name("ExtentMetres"), &invalidExtent, sizeof(invalidExtent))
	);
	const float rainfall = 20.0f;
	REQUIRE(store.SetProperty(provider, core::Name("PrecipitationRate"), &rainfall, sizeof(rainfall)));
	CHECK(store.Get<scene::WeatherCompute>(provider)->PrecipitationRate == rainfall);
	const uint32_t featureCells = 100;
	REQUIRE(store.SetProperty(provider, core::Name("FeatureCells"), &featureCells, sizeof(featureCells)));
	CHECK(store.Get<scene::WeatherCompute>(provider)->FeatureCells == 64);
	const int32_t negativeCount = -1;
	CHECK_FALSE(
		store.SetProperty(provider, core::Name("FeatureCells"), &negativeCount, sizeof(negativeCount))
	);
	const core::Name layerName("high-ice");
	REQUIRE(store.SetProperty(provider, core::Name("LayerName"), &layerName, sizeof(layerName)));
	CHECK(store.Get<scene::WeatherCompute>(provider)->LayerName == layerName);
	const core::Name tooLong(std::string(scene::MAX_WEATHER_LAYER_NAME_BYTES + 1, 'x'));
	CHECK_FALSE(store.SetProperty(provider, core::Name("LayerName"), &tooLong, sizeof(tooLong)));
	const core::Name empty;
	CHECK_FALSE(store.SetProperty(provider, core::Name("LayerName"), &empty, sizeof(empty)));
	const uint32_t invalidVersion = 9;
	CHECK_FALSE(
		store.SetProperty(provider, core::Name("AlgorithmVersion"), &invalidVersion, sizeof(invalidVersion))
	);
	const core::Name unknown("Unknown");
	CHECK_FALSE(store.SetProperty(provider, core::Name("Mode"), &unknown, sizeof(unknown)));
	const core::Name stateful("Stateful");
	REQUIRE(store.SetProperty(provider, core::Name("Mode"), &stateful, sizeof(stateful)));
	CHECK(store.Get<scene::WeatherCompute>(provider)->Mode == scene::WeatherMode::Stateful);
	store.GetMutable<scene::WeatherCompute>(provider)->DefinitionRevision = UINT32_MAX;
	const float clearing = 0.0f;
	REQUIRE(store.SetProperty(provider, core::Name("Coverage"), &clearing, sizeof(clearing)));
	CHECK(store.Get<scene::WeatherCompute>(provider)->DefinitionRevision == 1);
	store.SetAdoptOnly(true);
	CHECK_FALSE(store.SetProperty(provider, core::Name("Coverage"), &coverage, sizeof(coverage)));
}

TEST_CASE("first disabled cloud provider does not expose lower weather", "[scene][weather]") {
	ecs::Store store("weather.selection");
	scene::RegisterSceneClasses();
	const auto lighting = Lighting(store);
	const auto clouds = store.CreateInstance(ecs::Classes::Find(core::Name("Clouds")), "First");
	REQUIRE(store.SetParent(clouds, lighting));
	store.GetMutable<scene::Clouds>(clouds)->Enabled = false;
	const auto provider = Weather(store);
	REQUIRE(store.SetParent(provider, lighting));
	auto environment = scene::EnvironmentOf(store);
	CHECK(environment.CloudProvider == clouds);
	CHECK_FALSE(environment.HasWeatherCompute);
	store.DestroyInstance(clouds);
	store.GetMutable<scene::Clouds>(provider)->Enabled = false;
	environment = scene::EnvironmentOf(store);
	CHECK(environment.CloudProvider == provider);
	CHECK(environment.HasWeatherCompute);
	CHECK_FALSE(environment.CloudLayer.Enabled);
	store.GetMutable<scene::WeatherCompute>(provider)->Enabled = false;
	CHECK_FALSE(scene::EnvironmentOf(store).Weather.Enabled);
}

TEST_CASE("raw weather reads sanitize finite bounds without rewriting ECS", "[scene][weather]") {
	ecs::Store store("weather.clamp");
	const auto provider = Weather(store);
	REQUIRE(store.SetParent(provider, Lighting(store)));
	auto invalid = *store.Get<scene::WeatherCompute>(provider);
	invalid.CellSizeMetres = -1;
	invalid.Coverage = 12;
	invalid.BaseTemperatureCelsius = std::numeric_limits<float>::quiet_NaN();
	invalid.ExtentMetres = {0, -1, std::numeric_limits<float>::infinity()};
	invalid.FeatureCells = 31;
	invalid.VapourScale = 0.001f;
	invalid.VapourMixingRatio = 0.1f;
	store.Set(provider, invalid);
	CHECK_FALSE(scene::ValidWeatherCompute(invalid));
	const auto resolved = scene::EnvironmentOf(store).Weather;
	CHECK(scene::ValidWeatherCompute(resolved));
	CHECK(resolved.CellSizeMetres == 1);
	CHECK(resolved.Coverage == 1);
	CHECK(resolved.BaseTemperatureCelsius == scene::WeatherCompute{}.BaseTemperatureCelsius);
	CHECK(resolved.FeatureCells == 16);
	CHECK(resolved.VapourMixingRatio == resolved.VapourScale);
	CHECK(store.Get<scene::WeatherCompute>(provider)->CellSizeMetres == -1);
}

TEST_CASE("every authored weather control survives named snapshot serialization", "[scene][weather]") {
	ecs::Store store("weather.save");
	const auto provider = Weather(store);
	REQUIRE(store.SetParent(provider, Lighting(store)));
	scene::WeatherCompute authored;
	authored.CellSizeMetres = authored.CellSizeMetres == 0.0f ? 0.2f : authored.CellSizeMetres * 0.8f;
	authored.BaseHeightMetres = authored.BaseHeightMetres == 0.0f ? 0.2f : authored.BaseHeightMetres * 0.8f;
	authored.ThicknessMetres = authored.ThicknessMetres == 0.0f ? 0.2f : authored.ThicknessMetres * 0.8f;
	authored.ExtentMetres = {2048, 2048, 4096};
	authored.Seed = 345;
	authored.AlgorithmVersion = 1;
	authored.DefinitionRevision = 37;
	authored.Mode = scene::WeatherMode::Stateful;
	authored.Coverage = authored.Coverage == 0.0f ? 0.2f : authored.Coverage * 0.8f;
	authored.CondensateScale = authored.CondensateScale == 0.0f ? 0.2f : authored.CondensateScale * 0.8f;
	authored.VapourMixingRatio =
		authored.VapourMixingRatio == 0.0f ? 0.2f : authored.VapourMixingRatio * 0.8f;
	authored.VapourScale = authored.VapourScale == 0.0f ? 0.2f : authored.VapourScale * 0.8f;
	authored.BaseTemperatureCelsius =
		authored.BaseTemperatureCelsius == 0.0f ? 0.2f : authored.BaseTemperatureCelsius * 0.8f;
	authored.BasePressureHectopascals =
		authored.BasePressureHectopascals == 0.0f ? 0.2f : authored.BasePressureHectopascals * 0.8f;
	authored.TemperatureLapseCelsiusPerKilometre = authored.TemperatureLapseCelsiusPerKilometre == 0.0f
													   ? 0.2f
													   : authored.TemperatureLapseCelsiusPerKilometre * 0.8f;
	authored.TemperatureVariationCelsius =
		authored.TemperatureVariationCelsius == 0.0f ? 0.2f : authored.TemperatureVariationCelsius * 0.8f;
	authored.PressureVariationHectopascals =
		authored.PressureVariationHectopascals == 0.0f ? 0.2f : authored.PressureVariationHectopascals * 0.8f;
	authored.WindMetresPerSecond = {12, -3, 1};
	authored.TurbulenceMetresPerSecond =
		authored.TurbulenceMetresPerSecond == 0.0f ? 0.2f : authored.TurbulenceMetresPerSecond * 0.8f;
	authored.ShearMetresPerSecond =
		authored.ShearMetresPerSecond == 0.0f ? 0.2f : authored.ShearMetresPerSecond * 0.8f;
	authored.LiftMetresPerSecond =
		authored.LiftMetresPerSecond == 0.0f ? 0.2f : authored.LiftMetresPerSecond * 0.8f;
	authored.FeatureCells = 64;
	authored.FrontContrast = authored.FrontContrast == 0.0f ? 0.2f : authored.FrontContrast * 0.8f;
	authored.GrowthRate = authored.GrowthRate == 0.0f ? 0.2f : authored.GrowthRate * 0.8f;
	authored.TickRate = 30;
	authored.TimeScale = authored.TimeScale == 0.0f ? 0.2f : authored.TimeScale * 0.8f;
	authored.Paused = !authored.Paused;
	authored.MaintenanceSeconds =
		authored.MaintenanceSeconds == 0.0f ? 0.2f : authored.MaintenanceSeconds * 0.8f;
	authored.MaximumResidentBricks = 128;
	authored.MaximumLod = 3;
	authored.LodError = authored.LodError == 0.0f ? 0.2f : authored.LodError * 0.8f;
	authored.ShadowsEnabled = !authored.ShadowsEnabled;
	authored.ShadowStrength = authored.ShadowStrength == 0.0f ? 0.2f : authored.ShadowStrength * 0.8f;
	authored.ShadowDistanceMetres =
		authored.ShadowDistanceMetres == 0.0f ? 0.2f : authored.ShadowDistanceMetres * 0.8f;
	authored.ShadowResolution = 128;
	authored.ShadowCascades = 2;
	authored.ShadowUpdateSeconds =
		authored.ShadowUpdateSeconds == 0.0f ? 0.2f : authored.ShadowUpdateSeconds * 0.8f;
	authored.AtmosphereDensityStrength = 0.4f;
	authored.AtmosphereHazeStrength = 0.4f;
	authored.AtmosphereGlareSuppression = 0.4f;
	authored.FogStrength = authored.FogStrength == 0.0f ? 0.2f : authored.FogStrength * 0.8f;
	authored.PrecipitationRate =
		authored.PrecipitationRate == 0.0f ? 0.2f : authored.PrecipitationRate * 0.8f;
	authored.IceFraction = authored.IceFraction == 0.0f ? 0.2f : authored.IceFraction * 0.8f;
	authored.StormIntensity = authored.StormIntensity == 0.0f ? 0.2f : authored.StormIntensity * 0.8f;
	authored.HailIntensity = authored.HailIntensity == 0.0f ? 0.2f : authored.HailIntensity * 0.8f;
	authored.LightningRate = authored.LightningRate == 0.0f ? 0.2f : authored.LightningRate * 0.8f;
	authored.LayerName = core::Name("named-high-ice-layer");
	authored.Enabled = !authored.Enabled;
	authored.BoundaryX = scene::WeatherBoundary::Open;
	authored.BoundaryZ = scene::WeatherBoundary::Closed;
	authored.TopBoundary = scene::WeatherTopBoundary::Escape;
	authored.TerrainRevision = 4;
	authored.GroundVapourFluxKilogramsPerSquareMetreSecond = 0.003f;
	authored.RainFallSpeedMetresPerSecond = 12;

	REQUIRE(scene::ValidWeatherCompute(authored));
	store.Set(provider, authored);
	core::ByteWriter snapshot;
	REQUIRE(store.Save(snapshot));
	ecs::Store restored("weather.restored");
	core::ByteReader reader(snapshot.Bytes());
	REQUIRE(restored.Load(reader));
	REQUIRE(restored.Get<scene::WeatherCompute>(provider) != nullptr);
	CHECK(*restored.Get<scene::WeatherCompute>(provider) == authored);
	CHECK(scene::EnvironmentOf(restored).Weather == authored);
	const auto &descriptor = ecs::Components::Describe(ecs::Components::Of<scene::WeatherCompute>());
	core::ByteWriter encoded;
	descriptor.Write(encoded, &authored, 1);
	const std::string text(reinterpret_cast<const char *>(encoded.Bytes().data()), encoded.Bytes().size());
	CHECK(text.find("Stateful") != std::string::npos);
	CHECK_FALSE(descriptor.RawSerialisation);
}

TEST_CASE("malformed weather serialization refuses partial publication", "[scene][weather]") {
	scene::RegisterSceneClasses();
	const auto &descriptor = ecs::Components::Describe(ecs::Components::Of<scene::WeatherCompute>());
	scene::WeatherCompute original;
	original.Seed = 47;
	core::ByteWriter encoded;
	descriptor.Write(encoded, &original, 1);
	const std::vector<std::byte> valid(encoded.Bytes().begin(), encoded.Bytes().end());
	for (size_t count = 0; count < valid.size(); ++count) {
		scene::WeatherCompute destination;
		destination.Seed = 900;
		core::ByteReader reader{std::span(valid).first(count)};
		descriptor.Read(reader, &destination, 1);
		CHECK(reader.Failed());
		CHECK(destination.Seed == 900);
	}
	std::array<std::vector<std::byte>, 4> malformed{valid, valid, valid, valid};
	malformed[0][0] = std::byte{3};
	// First scalar becomes a quiet NaN in the explicit little-endian float encoding.
	malformed[1][4] = std::byte{0};
	malformed[1][5] = std::byte{0};
	malformed[1][6] = std::byte{0xc0};
	malformed[1][7] = std::byte{0x7f};
	// The final authored fall speed must also refuse a nonfinite value.
	malformed[2][valid.size() - 4] = std::byte{0};
	malformed[2][valid.size() - 3] = std::byte{0};
	malformed[2][valid.size() - 2] = std::byte{0xc0};
	malformed[2][valid.size() - 1] = std::byte{0x7f};
	const std::string validText(reinterpret_cast<const char *>(valid.data()), valid.size());
	const auto modeOffset = validText.find("Procedural");
	REQUIRE(modeOffset != std::string::npos);
	const std::string unknown = "BogusMode!";
	for (size_t index = 0; index < unknown.size(); ++index)
		malformed[3][modeOffset + index] = static_cast<std::byte>(unknown[index]);
	for (const auto &bytes : malformed) {
		scene::WeatherCompute destination;
		destination.Seed = 900;
		core::ByteReader reader(bytes);
		descriptor.Read(reader, &destination, 1);
		CHECK(reader.Failed());
		CHECK(destination.Seed == 900);
	}
	auto oversizedName = original;
	oversizedName.LayerName = core::Name(std::string(scene::MAX_WEATHER_LAYER_NAME_BYTES + 1, 'x'));
	core::ByteWriter oversized;
	descriptor.Write(oversized, &oversizedName, 1);
	scene::WeatherCompute preserved;
	preserved.Seed = 901;
	core::ByteReader oversizedReader(oversized.Bytes());
	descriptor.Read(oversizedReader, &preserved, 1);
	CHECK(oversizedReader.Failed());
	CHECK(preserved.Seed == 901);
	core::ByteWriter batch;
	descriptor.Write(batch, &original, 1);
	auto invalid = original;
	invalid.CellSizeMetres = 0;
	descriptor.Write(batch, &invalid, 1);
	std::array<scene::WeatherCompute, 2> destinations;
	destinations[0].Seed = 700;
	destinations[1].Seed = 800;
	core::ByteReader batchReader(batch.Bytes());
	descriptor.Read(batchReader, destinations.data(), destinations.size());
	CHECK(batchReader.Failed());
	CHECK(destinations[0].Seed == 700);
	CHECK(destinations[1].Seed == 800);
}

TEST_CASE("selected weather uses its replicated integer clock", "[scene][weather]") {
	ecs::Store store("weather.clock");
	const auto provider = Weather(store);
	REQUIRE(store.SetParent(provider, Lighting(store)));
	CHECK_FALSE(scene::EnvironmentOf(store).HasWeatherClock);
	scene::WeatherClock clock;
	clock.Tick = (uint64_t{1} << 54) + 3;
	clock.Fraction = 0.25;
	store.Set(provider, clock);
	const auto environment = scene::EnvironmentOf(store);
	CHECK(environment.HasWeatherClock);
	CHECK(environment.Clock.Tick == clock.Tick);
	CHECK(environment.Clock.Fraction == clock.Fraction);
	CHECK(environment.Clock.Tick != static_cast<uint64_t>(store.Time().Elapsed));
	scene::WeatherClockEstimate estimate;
	estimate.HistoryRevision = 7;
	store.Set(provider, estimate);
	CHECK(scene::EnvironmentOf(store).WeatherHistoryRevision == 7);
}

TEST_CASE(
	"accepted weather controls keep pending desired edits out of unfinished ticks", "[scene][weather]"
) {
	ecs::Store store("weather.accepted");
	const auto provider = Weather(store);
	REQUIRE(store.SetParent(provider, Lighting(store)));
	const scene::WeatherCompute original = *store.Get<scene::WeatherCompute>(provider);
	auto desired = original;
	desired.Seed = 21;
	desired.Coverage = 0.8f;
	desired.WindMetresPerSecond.X = 40;
	store.Set(provider, desired);
	CHECK(scene::WeatherControlsOf(store, provider)->Seed == desired.Seed);
	CHECK(scene::WeatherControlsOf(store, ecs::NULL_ENTITY) == nullptr);
	store.Set(provider, scene::WeatherAcceptedDefinition{original, "Lighting.Weather#old"});
	CHECK(scene::WeatherControlsOf(store, provider)->Seed == original.Seed);
	CHECK(scene::EnvironmentOf(store).Weather.Seed == desired.Seed);
	store.Set(provider, scene::WeatherClock{});
	CHECK(scene::EnvironmentOf(store).Weather == original);
	CHECK(*store.Get<scene::WeatherCompute>(provider) == desired);

	core::ByteWriter saved;
	REQUIRE(store.Save(saved));
	ecs::Store restored("weather.accepted.restored");
	core::ByteReader reader(saved.Bytes());
	REQUIRE(restored.Load(reader));
	CHECK(*restored.Get<scene::WeatherCompute>(provider) == desired);
	CHECK(restored.Get<scene::WeatherAcceptedDefinition>(provider)->Controls == original);
	CHECK(restored.Get<scene::WeatherAcceptedDefinition>(provider)->Layer == "Lighting.Weather#old");
	CHECK(scene::EnvironmentOf(restored).Weather == original);

	store.Set(provider, scene::WeatherAcceptedDefinition{desired});
	CHECK(scene::WeatherControlsOf(store, provider)->Seed == desired.Seed);
	CHECK(scene::EnvironmentOf(store).Weather == desired);
	store.Remove<scene::WeatherAcceptedDefinition>(provider);
	CHECK(scene::WeatherControlsOf(store, provider)->Seed == desired.Seed);
}

TEST_CASE(
	"accepted weather codecs refuse a malformed batch without replacing its prefix", "[scene][weather]"
) {
	scene::RegisterSceneClasses();
	const auto &descriptor =
		ecs::Components::Describe(ecs::Components::Of<scene::WeatherAcceptedDefinition>());
	std::array<scene::WeatherAcceptedDefinition, 2> source;
	source[0].Controls.Seed = 17;
	source[1].Controls.CellSizeMetres = 0;
	core::ByteWriter bytes;
	descriptor.Write(bytes, source.data(), source.size());
	std::array<scene::WeatherAcceptedDefinition, 2> destination;
	destination[0].Controls.Seed = 700;
	destination[1].Controls.Seed = 800;
	core::ByteReader reader(bytes.Bytes());
	descriptor.Read(reader, destination.data(), destination.size());
	CHECK(reader.Failed());
	CHECK(destination[0].Controls.Seed == 700);
	CHECK(destination[1].Controls.Seed == 800);
	CHECK_FALSE(descriptor.RawSerialisation);
}

TEST_CASE("accepted weather names have a bounded versioned transactional codec", "[scene][weather]") {
	scene::RegisterSceneClasses();
	const auto &descriptor =
		ecs::Components::Describe(ecs::Components::Of<scene::WeatherAcceptedDefinition>());
	scene::WeatherAcceptedDefinition maximum;
	maximum.Layer = std::string(scene::MAX_WEATHER_STREAM_LAYER_BYTES, 'x');
	core::ByteWriter encoded;
	descriptor.Write(encoded, &maximum, 1);
	scene::WeatherAcceptedDefinition restored;
	core::ByteReader valid(encoded.Bytes());
	descriptor.Read(valid, &restored, 1);
	REQUIRE_FALSE(valid.Failed());
	CHECK(restored.Controls == maximum.Controls);
	CHECK(restored.Layer == maximum.Layer);

	auto oversized = maximum;
	oversized.Layer += 'x';
	core::ByteWriter invalidBytes;
	descriptor.Write(invalidBytes, &oversized, 1);
	restored.Layer = "retained";
	core::ByteReader invalid(invalidBytes.Bytes());
	descriptor.Read(invalid, &restored, 1);
	CHECK(invalid.Failed());
	CHECK(restored.Layer == "retained");

	std::vector<std::byte> oldVersion(encoded.Bytes().begin(), encoded.Bytes().end());
	oldVersion[0] = std::byte{1};
	core::ByteReader obsolete(oldVersion);
	descriptor.Read(obsolete, &restored, 1);
	CHECK(obsolete.Failed());
	CHECK(restored.Layer == "retained");
	core::ByteReader truncated{encoded.Bytes().first(encoded.Bytes().size() - 1)};
	descriptor.Read(truncated, &restored, 1);
	CHECK(truncated.Failed());
	CHECK(restored.Layer == "retained");
}
