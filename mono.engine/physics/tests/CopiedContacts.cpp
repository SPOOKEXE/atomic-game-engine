#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/CopiedContacts.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.physics.copiedcontacts")
TEST_DEPENDS("engine.physics.query")

TEST_CASE(
	"copied floor edges preserve grounded travel while destination walls stop it",
	"[physics][copied-contacts]"
) {
	using namespace engine;
	const bool wall = GENERATE(false, true);
	CAPTURE(wall);
	ecs::Store source("near"), destination("far");
	physics::PreparePhysicsWorld(source);
	physics::PreparePhysicsWorld(destination);
	scene::PartDesc floor;
	floor.Frame = core::CFrame({0, -1, 0});
	floor.Size = {100, 2, 100};
	(void)scene::MakePart(source, floor);
	(void)scene::MakePart(destination, floor);
	if (wall) {
		scene::PartDesc obstacle;
		obstacle.Frame = core::CFrame({0, 2, -4.2f});
		obstacle.Size = {10, 4, .2f};
		(void)scene::MakePart(destination, obstacle);
	}
	physics::ContactWindow window;
	window.Centre = {0, 3, -3.2f};
	window.Normal = {0, 0, 1};
	window.First = {5, 0, 0};
	window.Second = {0, 5, 0};
	window.Depth = 4;
	physics::CopiedStaticContacts geometry;
	std::string failure;
	REQUIRE(physics::CollectStaticContacts(destination, window, geometry, failure));
	scene::PartDesc body;
	body.Frame = core::CFrame({0, 2.5f, -2.6666667f});
	body.Size = {2, 5, 1};
	body.Simulated = true;
	const auto root = scene::MakePart(source, body);
	physics::SyncBroadphase(source);
	source.AdvanceTick(1.f / 60);
	physics::SetCopiedBodyContacts(source, {{root, std::move(geometry), true}});
	physics::BeginCopiedContactStep(source);
	const core::Vector3 displacement{0, -.0109f, -2};
	auto integrated = body.Frame;
	integrated.Position = integrated.Position + displacement;
	source.Set(root, scene::Transform{integrated});
	source.Set(root, scene::Motion{displacement * 60, {}});
	physics::SolveCopiedContactStep(source);
	const auto position = source.Get<scene::Transform>(root)->Frame.Position;
	CAPTURE(position.Y, position.Z);
	CHECK(position.Y >= 2.499f);
	if (wall) {
		CHECK(position.Z > -3.61f);
		CHECK(position.Z < -3.5f);
		CHECK(source.Get<scene::Motion>(root)->Linear.Z == Catch::Approx(0).margin(.001f));
	} else {
		CHECK(position.Z == Catch::Approx(integrated.Position.Z).margin(.001f));
		CHECK(source.Get<scene::Motion>(root)->Linear.Z == Catch::Approx(-120));
	}
}
