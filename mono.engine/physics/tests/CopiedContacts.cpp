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

TEST_CASE("copied static contacts retain physical material across the wire", "[physics][copied-contacts]") {
	using namespace engine;
	ecs::Store destination("far");
	physics::PreparePhysicsWorld(destination);
	scene::PartDesc floor;
	floor.Frame = core::CFrame({0, -1, 0});
	floor.Size = {10, 2, 10};
	const auto entity = scene::MakePart(destination, floor);
	scene::PhysicsProperties material;
	material.Custom = true;
	material.Friction = 0.8f;
	material.Elasticity = 0.35f;
	destination.Set(entity, material);
	physics::ContactWindow window;
	window.Centre = {0, 0, 0};
	window.Normal = {0, 0, 1};
	window.First = {5, 0, 0};
	window.Second = {0, 5, 0};
	window.Depth = 5;
	physics::CopiedStaticContacts copied;
	std::string failure;
	REQUIRE(physics::CollectStaticContacts(destination, window, copied, failure));
	REQUIRE(copied.Shapes.size() == 1);
	CHECK(copied.Shapes[0].Friction == Catch::Approx(0.8f));
	CHECK(copied.Shapes[0].Restitution == Catch::Approx(0.35f));
	core::ByteWriter writer;
	REQUIRE(physics::WriteCopiedContacts(writer, copied));
	core::ByteReader reader(writer.Bytes());
	physics::CopiedStaticContacts restored;
	REQUIRE(physics::ReadCopiedContacts(reader, restored));
	CHECK(reader.Remaining() == 0);
	REQUIRE(restored.Shapes.size() == 1);
	CHECK(restored.Shapes[0].Friction == Catch::Approx(0.8f));
	CHECK(restored.Shapes[0].Restitution == Catch::Approx(0.35f));
}

TEST_CASE(
	"dynamic portal contacts keep stable bodies and finite apertures over the wire", "[physics][portal]"
) {
	using namespace engine;
	ecs::Store destination("far");
	physics::PreparePhysicsWorld(destination);
	scene::PartDesc body;
	body.Frame = core::CFrame({0, 0, -1});
	body.Size = {2, 2, 2};
	body.Simulated = true;
	const ecs::Entity dynamic = scene::MakePart(destination, body);
	destination.Set(dynamic, scene::Motion{{3, 0, 0}, {0, 2, 0}});
	REQUIRE(scene::ConfigureBodyIdentityAuthority(destination, 0x52));
	scene::BodyIdentity identity;
	REQUIRE(scene::EnsureBodyIdentity(destination, dynamic, identity));
	physics::ContactWindow window;
	window.Centre = {0, 0, 0};
	window.Normal = {0, 0, 1};
	window.First = {4, 0, 0};
	window.Second = {0, 4, 0};
	window.Depth = 4;
	physics::CopiedDynamicContacts copied;
	std::string failure;
	REQUIRE(physics::CollectDynamicContacts(destination, window, copied, failure));
	REQUIRE(copied.Bodies.size() == 1);
	CHECK(copied.Bodies[0].Identity.Key.IsValid());
	CHECK(copied.Bodies[0].Motion.Linear.X == Catch::Approx(3));
	core::ByteWriter writer;
	REQUIRE(physics::WriteCopiedDynamicContacts(writer, copied));
	physics::CopiedDynamicContacts restored;
	core::ByteReader reader(writer.Bytes());
	REQUIRE(physics::ReadCopiedDynamicContacts(reader, restored));
	CHECK(reader.Remaining() == 0);
	REQUIRE(restored.Bodies.size() == 1);
	CHECK(restored.Bodies[0].Identity.Key == copied.Bodies[0].Identity.Key);
	CHECK(restored.Bodies[0].Identity.Generation == copied.Bodies[0].Identity.Generation);
	CHECK(restored.Bodies[0].Window.Depth == Catch::Approx(4));
}

TEST_CASE("copied static contact restitution changes the source motion", "[physics][copied-contacts]") {
	using namespace engine;
	ecs::Store source("near");
	physics::PreparePhysicsWorld(source);
	scene::PartDesc body;
	body.Frame = core::CFrame({0, 0, 0});
	body.Size = {1, 1, 1};
	body.Simulated = true;
	const auto root = scene::MakePart(source, body);
	physics::SyncBroadphase(source);
	source.AdvanceTick(1.f / 60);
	physics::CopiedContactShape wall;
	wall.Frame = core::CFrame({2, 0, 0});
	wall.Extent = {0.5f, 2, 2};
	wall.Restitution = 1;
	physics::SetCopiedBodyContacts(source, {{root, {{wall}}, true}});
	physics::BeginCopiedContactStep(source);
	source.Set(root, scene::Transform{core::CFrame({3, 0, 0})});
	source.Set(root, scene::Motion{{180, 0, 0}, {}});
	physics::SolveCopiedContactStep(source);
	CHECK(source.Get<scene::Motion>(root)->Linear.X == Catch::Approx(-180).margin(0.1f));
	CHECK(source.Get<scene::Transform>(root)->Frame.Position.X < 1.1f);
}

TEST_CASE("copied kinematic support transfers its contact velocity", "[physics][copied-contacts]") {
	using namespace engine;
	ecs::Store source("near");
	physics::PreparePhysicsWorld(source);
	scene::PartDesc body;
	body.Frame = core::CFrame({0, 0, 0});
	body.Size = {1, 1, 1};
	body.Simulated = true;
	const auto root = scene::MakePart(source, body);
	physics::SyncBroadphase(source);
	source.AdvanceTick(1.f / 60);
	physics::CopiedContactShape wall;
	wall.Frame = core::CFrame({2, 0, 0});
	wall.Extent = {0.5f, 2, 2};
	wall.Restitution = 1;
	wall.Linear = {60, 0, 0};
	physics::SetCopiedBodyContacts(source, {{root, {{wall}}, true}});
	physics::BeginCopiedContactStep(source);
	source.Set(root, scene::Transform{core::CFrame({3, 0, 0})});
	source.Set(root, scene::Motion{{180, 0, 0}, {}});
	physics::SolveCopiedContactStep(source);

	// The bounce happens in the support's moving frame: 180 - 60 becomes -120,
	// then the support velocity is restored for a source-world result of -60.
	CHECK(source.Get<scene::Motion>(root)->Linear.X == Catch::Approx(-60).margin(0.1f));
}

TEST_CASE("two contact windows for one body block ambiguous seam motion", "[physics][copied-contacts]") {
	using namespace engine;
	ecs::Store source("near");
	physics::PreparePhysicsWorld(source);
	scene::PartDesc body;
	body.Simulated = true;
	const auto root = scene::MakePart(source, body);
	source.AdvanceTick(1.f / 60);
	physics::SetCopiedBodyContacts(source, {{root, {}, true}, {root, {}, true}});
	physics::BeginCopiedContactStep(source);
	source.Set(root, scene::Transform{core::CFrame({1, 0, 0})});
	source.Set(root, scene::Motion{{60, 0, 0}, {}});
	physics::SolveCopiedContactStep(source);
	CHECK(source.Get<scene::Transform>(root)->Frame.Position.X == Catch::Approx(0));
	CHECK(source.Get<scene::Motion>(root)->Linear.X == Catch::Approx(0));
}

TEST_CASE("copied rotated floor and ceiling stop a fast body", "[physics][copied-contacts]") {
	using namespace engine;
	const bool ceiling = GENERATE(false, true);
	ecs::Store source("near"), destination("far");
	physics::PreparePhysicsWorld(source);
	physics::PreparePhysicsWorld(destination);
	scene::PartDesc surface;
	surface.Frame = core::CFrame::Angles(0, 0, 0.4f);
	surface.Size = {10, 0.5f, 10};
	(void)scene::MakePart(destination, surface);
	physics::ContactWindow window;
	window.Centre = {0, 0, 5};
	window.Normal = {0, 0, 1};
	window.First = {10, 0, 0};
	window.Second = {0, 10, 0};
	window.Depth = 10;
	physics::CopiedStaticContacts contacts;
	std::string failure;
	REQUIRE(physics::CollectStaticContacts(destination, window, contacts, failure));
	REQUIRE_FALSE(contacts.Shapes.empty());
	scene::PartDesc body;
	body.Frame = core::CFrame({0, ceiling ? -3.0f : 3.0f, 0});
	body.Size = {1, 1, 1};
	body.Simulated = true;
	const auto root = scene::MakePart(source, body);
	physics::SyncBroadphase(source);
	source.AdvanceTick(1.f / 60);
	physics::SetCopiedBodyContacts(source, {{root, std::move(contacts), true}});
	physics::BeginCopiedContactStep(source);
	const float velocity = ceiling ? 600.0f : -600.0f;
	source.Set(root, scene::Transform{core::CFrame({0, ceiling ? 7.0f : -7.0f, 0})});
	source.Set(root, scene::Motion{{0, velocity, 0}, {}});
	physics::SolveCopiedContactStep(source);
	const float finalHeight = source.Get<scene::Transform>(root)->Frame.Position.Y;
	if (ceiling)
		CHECK(finalHeight < -0.4f);
	else
		CHECK(finalHeight > 0.4f);
}
