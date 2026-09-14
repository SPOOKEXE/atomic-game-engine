#include "../src/DataFactoryPausedPresentation.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/Compositor.hpp>

TEST_SUITE_ID("client.data-factory.paused-presentation")

TEST_CASE(
	"paused data-factory collection publishes one current-tick labeled view", "[client][data-factory]"
) {
	using namespace engine;
	scene::EnsureClassTree();
	render::RegisterPresentationComponents();
	world::Universe worlds;
	const world::WorldId world = worlds.Create({.Name = core::Name("paused-data-factory")});
	uint64_t expectedTick = 0;
	worlds.Enter(world, [&](ecs::Store &store) {
		store.SetResource(render::DrawList{});
		const ecs::Entity workspace = scene::InstallServices(store);
		scene::PartDesc partDesc;
		partDesc.Frame = core::CFrame(core::Vector3{1, 2, 3});
		const ecs::Entity part = scene::MakePart(store, partDesc);
		REQUIRE(store.SetParent(part, workspace));
		const auto label = [&store, part](std::string_view name, std::string_view value) {
			ecs::AttributeValue attribute;
			attribute.Type = ecs::PropertyType::String;
			attribute.String = value;
			REQUIRE(ecs::SetAttribute(store, part, core::Name(name), attribute));
		};
		label("DataFactoryId", "fixture/paused");
		label("DataFactorySemanticId", "fixture/box");
		label("DataFactoryPartId", "fixture/part/paused");
		REQUIRE(scene::SyncRendered(store) == 1);

		const ecs::Entity eye = store.CreateInstance(scene::CameraClass(), "Eye");
		store.Set(eye, scene::Transform{core::CFrame(core::Vector3{4, 5, 6})});
		store.Set(eye, scene::Camera{});
		store.SetResource(scene::ActiveCamera{eye});
		store.AdvanceTick(1.0f / 60.0f);
		render::CollectInstances(store, render::DrawCollectionTime::CurrentTick);
		expectedTick = store.Time().Tick;
	});
	core::ByteWriter before;
	REQUIRE(worlds.Save(before));

	client::Compositor rejected;
	worlds.Enter(world, [&](ecs::Store &store) {
		CHECK_FALSE(client::PublishPausedDataFactoryPresentation(store, rejected, world));
	});

	client::Compositor views;
	views.Track(world, core::Name("paused-data-factory"), 1);
	std::optional<client::PausedDataFactoryPresentation> paused;
	worlds.Enter(world, [&](ecs::Store &store) {
		paused = client::PublishPausedDataFactoryPresentation(store, views, world);
		CHECK(store.Time().Tick == expectedTick);
	});
	core::ByteWriter after;
	REQUIRE(worlds.Save(after));
	CHECK(before.Bytes().size() == after.Bytes().size());
	CHECK(std::equal(before.Bytes().begin(), before.Bytes().end(), after.Bytes().begin()));

	REQUIRE(paused);
	CHECK(paused->Tick == expectedTick);
	CHECK(paused->Frame.Position == core::Vector3{4, 5, 6});
	REQUIRE(paused->ObjectLabels.size() == 1);
	CHECK(paused->ObjectLabels.front().StableId == "fixture/paused");
	REQUIRE(paused->SemanticLabels.size() == 1);
	CHECK(paused->SemanticLabels.front().StableId == "fixture/box");
	REQUIRE(paused->PartLabels.size() == 1);
	CHECK(paused->PartLabels.front().StableId == "fixture/part/paused");

	views.Compose(0.0f, world);
	REQUIRE(views.Instances().size() == 1);
	CHECK(views.CameraFrame().Position == core::Vector3{4, 5, 6});
	REQUIRE(views.Views().size() == 1);
	CHECK(views.Views().front().Header.SourceTick == paused->Tick);
	CHECK(views.Views().front().Header.Alpha == 1.0f);
}
