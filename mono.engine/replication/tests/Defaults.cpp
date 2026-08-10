// The one table three programs used to keep separately.
//
// **Nothing in the build compared the copies, and that is what this replaces.**
// `DEFERRED.md` D00018 said the risk was a component whose detector differs
// between two of the three — "the second is the one that bites without warning,
// because the copies are in three programs and nothing in the build compares
// them, and the symptom is a value that crosses in the studio and not on a
// server". By v0.13 the harness had drifted by three rows and nothing had
// noticed.
//
// So this suite is not really about the values. It is about there being one
// place to hold them and a case that fails when somebody changes it without
// meaning to.

#include <engine/replication/Defaults.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

TEST_SUITE_ID("engine.replication.defaults")

using engine::replication::ChangeDetection;
using engine::replication::DefaultReplicatedComponents;

namespace {
	// What the table says about one name, or nothing when it says nothing.
	const engine::replication::ReplicatedComponent *Row(std::string_view name) {
		for (const auto &component : DefaultReplicatedComponents()) {
			if (component.Name == name) {
				return &component;
			}
		}
		return nullptr;
	}
}

TEST_CASE("the pairing is the one the versions argued for", "[replication][defaults]") {
	// **Each of these is a decision with a bug behind it**, and pinning them is
	// how the argument survives somebody tidying the table.
	//
	// Observed: written every tick by a system, so the dirty bits already know
	// and hashing would be a pass over the world to learn what was free.
	REQUIRE(Row("scene.Transform") != nullptr);
	CHECK(Row("scene.Transform")->Detection == ChangeDetection::Observed);

	REQUIRE(Row("scene.Motion") != nullptr);
	CHECK(Row("scene.Motion")->Detection == ChangeDetection::Observed);

	// Signed: written once by a script and then never. Observing them buys a
	// dirty column paid every tick and read never — and *not* signing them is
	// the v0.7 bug where a part recoloured at runtime kept its old colour on
	// every client for ever.
	REQUIRE(Row("scene.Visual") != nullptr);
	CHECK(Row("scene.Visual")->Detection == ChangeDetection::Signature);

	REQUIRE(Row("scene.Bounds") != nullptr);
	CHECK(Row("scene.Bounds")->Detection == ChangeDetection::Signature);
}

TEST_CASE("a replica has everything it needs to draw a scene", "[replication][defaults]") {
	// **Each absence here was a visible failure rather than a missing feature**,
	// which is why the list is asserted by name rather than by count.
	//
	// Without a size a client that received a position has nothing to draw;
	// without a surface an imported mesh is untextured, which reads as a broken
	// texture path rather than as a component nobody sent; without the mirror,
	// the lens and the parent link a reflection does not exist on a replica at
	// all, because a surface camera is aimed off the part it is parented to.
	for (const std::string_view needed : {
			 std::string_view("scene.Transform"),
			 std::string_view("scene.Bounds"),
			 std::string_view("scene.Visual"),
			 std::string_view("scene.SurfaceAppearance"),
			 std::string_view("scene.Tags"),
			 std::string_view("scene.SurfaceCamera"),
			 std::string_view("scene.Camera"),
			 std::string_view("ecs.Hierarchy"),
		 }) {
		INFO("component: " << needed);
		CHECK(Row(needed) != nullptr);
	}
}

TEST_CASE("no component is declared twice", "[replication][defaults]") {
	// A duplicate row is two answers to how a component's changes are found,
	// and `Authority::Replicate` takes the last one — so it would be a detector
	// chosen by list order, which is exactly the silent kind of wrong this
	// table exists to remove.
	const auto table = DefaultReplicatedComponents();
	CHECK_FALSE(table.empty());

	for (size_t outer = 0; outer < table.size(); outer++) {
		for (size_t inner = outer + 1; inner < table.size(); inner++) {
			INFO("component: " << table[outer].Name);
			CHECK(table[outer].Name != table[inner].Name);
		}
	}
}

TEST_CASE("every name is qualified by the module that owns it", "[replication][defaults]") {
	// These are the strings `ecs::Components` registers, and an unqualified one
	// would be a name no store resolves — a row that declares nothing and
	// reports nothing, which is the failure mode this whole entry is about.
	for (const auto &component : DefaultReplicatedComponents()) {
		INFO("component: " << component.Name);
		CHECK(component.Name.find('.') != std::string_view::npos);
		CHECK_FALSE(component.Name.empty());
	}
}
