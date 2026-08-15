// Every component every module this program links registers, against the
// serialisation rules.
//
// **The wide sweep.** Each module's own suite asks `AuditComponents` about its
// own prefix, which is what says *which* module broke a rule. This one asks
// about all of them at once, from the binary that links the most of them, and
// is what says nothing escaped: a module with no suite of its own, a component
// registered from a path nobody thought of as registration, and the
// compiler-spelled names `Components::Of<T>()` mints for a type nobody named.
//
// That last set is the one worth reading twice. `Of<T>()` registers under the
// compiler's spelling of the type and installs the raw object representation as
// the serialisation, so a struct with padding that nothing ever *declared* as a
// component still reaches a save file through it. There is no list of those
// anywhere, which is exactly why this asks the registry rather than a list.

#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Invariants.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>

TEST_SUITE_ID("client.componentinvariants")
TEST_DEPENDS("engine.ecs.invariants")

TEST_CASE("every component this program registers obeys the serialisation rules", "[client][invariants]") {
	engine::ecs::RegisterAttributeComponents();
	engine::scene::RegisterSceneComponents();
	engine::gui::RegisterGuiComponents();
	engine::script::RegisterScriptComponents();
	engine::effects::RegisterEffectComponents();
	engine::physics::RegisterPhysicsComponents();
	engine::replication::RegisterReplicationComponents();
	engine::world::RegisterMailboxTypes();
	client::RegisterClientComponents();

	// **Scene classes as well as scene components**, because declaring a class
	// is what first reaches for several of the types this asks about, and a
	// component only in the registry once something has touched it is a
	// component this sweep would otherwise miss.
	engine::scene::RegisterSceneClasses();

	CHECK(engine::ecs::Describe(engine::ecs::AuditComponents()) == "");
}

TEST_CASE("every declared property obeys the class table's rules", "[client][invariants]") {
	// The other half of the sweep above, and the one that covers what a script
	// touches rather than what a file carries. `engine.ecs.invariants` is where
	// each rule is proved to fire.
	engine::scene::RegisterSceneClasses();
	(void)engine::gui::RegisterGuiClasses();
	(void)engine::script::ScriptClass();

	CHECK(engine::ecs::Describe(engine::ecs::AuditProperties()) == "");
}
