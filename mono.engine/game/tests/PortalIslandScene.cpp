#include <engine/ecs/Store.hpp>
#include <engine/game/PortalIslandScene.hpp>
#include <engine/game/PortalSeamCoordinator.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.game.portalislandscene")
TEST_DEPENDS("engine.physics.portalisland")

namespace {
	engine::physics::ContactWindow Window() {
		engine::physics::ContactWindow window;
		window.Centre = {0, 0, 0};
		window.Normal = {1, 0, 0};
		window.First = {0, 4, 0};
		window.Second = {0, 0, 4};
		window.Depth = 4;
		return window;
	}

	engine::physics::CopiedDynamicContact Body(uint64_t key, float position, float velocity) {
		using namespace engine;
		physics::CopiedDynamicContact body;
		body.Identity.Key.Low = key;
		body.Identity.Generation = 1;
		body.Frame = core::CFrame({position, 0, 0});
		body.Motion.Linear = {velocity, 0, 0};
		body.Extent = {1, 1, 1};
		body.Mass = 1;
		body.Window = Window();
		return body;
	}
}

TEST_CASE("two worlds exchange one finite dynamic portal contact", "[game][portal][island]") {
	using namespace engine;
	// The two snapshots stand in for independently owned stores. They meet only
	// as copied values at the fixed-step barrier.
	const physics::CopiedDynamicContact near = Body(11, -0.5f, 4.0f);
	const physics::CopiedDynamicContact far = Body(22, 0.5f, -4.0f);
	physics::PortalIslandPacket packet;
	REQUIRE(game::AppendPortalIslandContact(near, far, packet));
	REQUIRE(packet.Bodies.size() == 2);
	REQUIRE(packet.Contacts.size() == 1);

	std::vector<physics::PortalIslandResult> results;
	REQUIRE(
		physics::SolvePortalIsland(packet.Bodies, packet.Contacts, results) ==
		physics::PortalIslandStatus::Complete
	);
	REQUIRE(results.size() == 2);
	CHECK(results[0].LinearVelocity.X == Catch::Approx(0));
	CHECK(results[1].LinearVelocity.X == Catch::Approx(0));
}

TEST_CASE("the seam barrier collects a copied far body into a local impulse", "[game][portal][island]") {
	using namespace engine;
	ecs::Store near("near");
	REQUIRE(scene::ConfigureBodyIdentityAuthority(near, 0x11));
	scene::PartDesc part;
	part.Frame = core::CFrame({-0.5f, 0, 0});
	part.Size = {2, 2, 2};
	part.Simulated = true;
	const ecs::Entity root = scene::MakePart(near, part);
	near.Set(root, scene::Motion{{4, 0, 0}, {}});
	scene::BodyIdentity identity;
	REQUIRE(scene::EnsureBodyIdentity(near, root, identity));
	scene::PortalCrossingState crossing;
	crossing.Phase = scene::PortalCrossingPhase::Overlapping;
	near.Set(root, crossing);

	physics::CopiedDynamicContact far = Body(22, 0.5f, -4.0f);
	physics::SetCopiedDynamicBodyContacts(near, {{root, {{far}}, true}});
	const auto callbacks = game::MakePortalIslandCoordinator().Callbacks();
	std::vector<std::byte> payload;
	callbacks.Collect(near, payload);
	physics::PortalIslandPacket packet;
	REQUIRE(physics::ReadPortalIslandPacket(payload, packet));
	REQUIRE(packet.Contacts.size() == 1);
	REQUIRE(packet.Bodies.size() == 2);
	CHECK(packet.Bodies[0].Owned);
	CHECK_FALSE(packet.Bodies[1].Owned);

	std::vector<world::FixedStepBarrierRecord> records;
	records.push_back({"near", std::move(payload)});
	std::vector<world::FixedStepBarrierRecord> replies;
	REQUIRE(callbacks.Resolve(records, replies));
	REQUIRE(replies.size() == 1);
	REQUIRE(callbacks.Apply(near, replies.front().Payload));
	CHECK(near.Get<scene::Motion>(root)->Linear.X == Catch::Approx(0));
}
