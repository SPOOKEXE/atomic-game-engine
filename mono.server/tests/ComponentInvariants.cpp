// The components this program registers, against the serialisation rules.
//
// `client.componentinvariants` is the same sweep from the other side of the
// wire, and between them they cover every module either program links. This one
// is what covers `server.` and the placeholder world, which no client binary
// can reach.

#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Invariants.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>

#include <catch2/catch_test_macros.hpp>

#include <server/Simulation.hpp>

TEST_SUITE_ID("server.componentinvariants")
TEST_DEPENDS("engine.ecs.invariants")

TEST_CASE("every component this program registers obeys the serialisation rules", "[server][invariants]") {
	engine::ecs::RegisterAttributeComponents();
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::physics::RegisterPhysicsComponents();
	engine::replication::RegisterReplicationComponents();
	engine::world::RegisterMailboxTypes();
	server::RegisterPlaceholderComponents();

	CHECK(engine::ecs::Describe(engine::ecs::AuditComponents()) == "");
}

TEST_CASE("every declared property obeys the class table's rules", "[server][invariants]") {
	engine::scene::RegisterSceneClasses();

	CHECK(engine::ecs::Describe(engine::ecs::AuditProperties()) == "");
}
