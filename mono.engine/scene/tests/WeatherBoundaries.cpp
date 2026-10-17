#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/WeatherCompute.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.scene.weatherboundaries")
TEST_DEPENDS("engine.scene.weathercompute")

using namespace engine;

TEST_CASE(
	"authored weather boundaries use named properties and persist complete physical controls",
	"[scene][weather]"
) {
	scene::RegisterSceneClasses();
	ecs::Store store("weather.boundaries");
	const auto provider = store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Weather");
	const core::Name open("Open"), closed("Closed"), escape("Escape"), unknown("Unknown");
	REQUIRE(store.SetProperty(provider, core::Name("BoundaryX"), &open, sizeof(open)));
	REQUIRE(store.SetProperty(provider, core::Name("BoundaryZ"), &closed, sizeof(closed)));
	REQUIRE(store.SetProperty(provider, core::Name("TopBoundary"), &escape, sizeof(escape)));
	const uint32_t revision = 12;
	const float flux = 0.002f, speed = 12;
	REQUIRE(store.SetProperty(provider, core::Name("TerrainRevision"), &revision, sizeof(revision)));
	REQUIRE(store.SetProperty(
		provider, core::Name("GroundVapourFluxKilogramsPerSquareMetreSecond"), &flux, sizeof(flux)
	));
	REQUIRE(store.SetProperty(provider, core::Name("RainFallSpeedMetresPerSecond"), &speed, sizeof(speed)));
	const auto controls = *store.Get<scene::WeatherCompute>(provider);
	CHECK(controls.BoundaryX == scene::WeatherBoundary::Open);
	CHECK(controls.BoundaryZ == scene::WeatherBoundary::Closed);
	CHECK(controls.TopBoundary == scene::WeatherTopBoundary::Escape);
	CHECK(controls.TerrainRevision == revision);
	CHECK(controls.GroundVapourFluxKilogramsPerSquareMetreSecond == flux);
	CHECK(controls.RainFallSpeedMetresPerSecond == speed);
	for (const auto name : {"BoundaryX", "BoundaryZ", "TopBoundary"}) {
		CHECK_FALSE(store.SetProperty(provider, core::Name(name), &unknown, sizeof(unknown)));
		CHECK(*store.Get<scene::WeatherCompute>(provider) == controls);
	}
	core::Name read;
	REQUIRE(store.GetProperty(provider, core::Name("BoundaryX"), &read, sizeof(read)));
	CHECK(read == open);
	core::ByteWriter bytes;
	REQUIRE(store.Save(bytes));
	ecs::Store restored("weather.boundaries.restored");
	core::ByteReader reader(bytes.Bytes());
	REQUIRE(restored.Load(reader));
	CHECK(*restored.Get<scene::WeatherCompute>(provider) == controls);
	store.SetAdoptOnly(true);
	CHECK_FALSE(store.SetProperty(provider, core::Name("BoundaryX"), &closed, sizeof(closed)));
}

TEST_CASE("weather terrain and fall controls have finite explicit bounds", "[scene][weather]") {
	scene::WeatherCompute controls;
	controls.BoundaryX = static_cast<scene::WeatherBoundary>(255);
	controls.BoundaryZ = static_cast<scene::WeatherBoundary>(255);
	controls.TopBoundary = static_cast<scene::WeatherTopBoundary>(255);
	controls.TerrainRevision = 0;
	controls.GroundVapourFluxKilogramsPerSquareMetreSecond = 2;
	controls.RainFallSpeedMetresPerSecond = -1;
	CHECK_FALSE(scene::ValidWeatherCompute(controls));
	const auto bounded = scene::ClampedWeatherCompute(controls);
	CHECK(scene::ValidWeatherCompute(bounded));
	CHECK(bounded.BoundaryX == scene::WeatherBoundary::Periodic);
	CHECK(bounded.BoundaryZ == scene::WeatherBoundary::Periodic);
	CHECK(bounded.TopBoundary == scene::WeatherTopBoundary::Closed);
	CHECK(bounded.TerrainRevision == 1);
	CHECK(bounded.GroundVapourFluxKilogramsPerSquareMetreSecond == 1);
	CHECK(bounded.RainFallSpeedMetresPerSecond == 0);
}

TEST_CASE("unknown serialized weather boundary names refuse transactional publication", "[scene][weather]") {
	scene::RegisterSceneClasses();
	const auto &descriptor = ecs::Components::Describe(ecs::Components::Of<scene::WeatherCompute>());
	scene::WeatherCompute controls;
	controls.BoundaryX = scene::WeatherBoundary::Open;
	core::ByteWriter writer;
	descriptor.Write(writer, &controls, 1);
	std::vector<std::byte> bytes(writer.Bytes().begin(), writer.Bytes().end());
	const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	const size_t offset = text.find("Open");
	REQUIRE(offset != std::string::npos);
	const std::string unknown = "Nope";
	for (size_t index = 0; index < unknown.size(); ++index)
		bytes[offset + index] = static_cast<std::byte>(unknown[index]);
	scene::WeatherCompute destination;
	destination.Seed = 47;
	const auto published = destination;
	core::ByteReader reader(bytes);
	descriptor.Read(reader, &destination, 1);
	CHECK(reader.Failed());
	CHECK(destination == published);
}
