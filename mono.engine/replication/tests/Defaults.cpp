// Which components a world sends, and the much shorter list of which it keeps.
//
// **Nothing in the build compared the copies, and that is what this replaces.**
// `DEFERRED.md` D00018 said the risk was a component whose detector differs
// between two of the three programs that each kept this table — "the copies are
// in three programs and nothing in the build compares them, and the symptom is a
// value that crosses in the studio and not on a server". By v0.13 the diagnostic
// harness had drifted by three rows and nothing had noticed.
//
// **This suite tests the rule and not `scene`'s component list**, because
// `replication` does not link `scene` and must not: the whole property that
// keeps `net` and `replication` separable is that neither knows what a component
// *is*. So the components below are registered here, under `scene.` names, to
// exercise the prefix, the exclusions and the detector — and what the real scene
// holds is `scene`'s business, asserted where both are linked.

#include <engine/replication/Defaults.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_SUITE_ID("engine.replication.defaults")

using engine::replication::ChangeDetection;
using engine::replication::DefaultReplicatedComponents;
using engine::replication::LocalToTheClient;

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

TEST_CASE("the client chooses its own eye, and lenses still cross", "[replication][defaults]") {
	// **The exclusion this list exists for, and it is narrower than it sounds.**
	// A replica may not mint an authoritative entity, so
	// `client::AimReplicaViewer` puts a *predicted* camera in the world and
	// points `ActiveCamera` at it. A replicated `ActiveCamera` would be a second
	// answer to which eye the world is seen through, and the two would fight
	// every frame.
	CHECK(LocalToTheClient("scene.ActiveCamera"));
	CHECK(LocalToTheClient("scene.CameraController"));

	// **`Camera` itself is a lens and must cross.** A `SurfaceCamera` carries
	// one, so a mirror with no replicated `Camera` cannot be aimed at all —
	// `AimSurfaceCameras` finds nothing and the pane is a flat grey rectangle on
	// every client. Excluding it broke `studio.playlink`'s "a mirror arrives on
	// the client whole", which is the case that exists to catch this.
	CHECK_FALSE(LocalToTheClient("scene.Camera"));
	CHECK_FALSE(LocalToTheClient("scene.SurfaceCamera"));
}

TEST_CASE("what a machine works out for itself is not sent", "[replication][defaults]") {
	// Each of these is rebuilt by whichever machine draws, so sending it is wire
	// spent on a value the far side is about to overwrite.
	CHECK(LocalToTheClient("scene.PreviousTransform"));
	CHECK(LocalToTheClient("scene.Rendered"));
	CHECK(LocalToTheClient("scene.QuickHash"));
	CHECK(LocalToTheClient("scene.Transient"));

	// A client's own input and its own identity. Sending the server's copy would
	// tell every client what some other machine is pressing.
	CHECK(LocalToTheClient("scene.InputState"));
	CHECK(LocalToTheClient("scene.LocalPlayer"));

	// And the ordinary case: everything else is shared.
	CHECK_FALSE(LocalToTheClient("scene.Transform"));
	CHECK_FALSE(LocalToTheClient("scene.Visual"));
	CHECK_FALSE(LocalToTheClient("ecs.Hierarchy"));
}

TEST_CASE("the set is derived, and never contradicts the exclusions", "[replication][defaults]") {
	// **The shape this is really about.** It was a hand-kept allow-list of nine
	// names, so a component added to `scene` did not cross until somebody
	// remembered — and "somebody remembered" is exactly what three copies of the
	// table had already failed at. The question a host answers now is which
	// components are local, which is the much shorter list.
	//
	// **What is asserted is the derivation and not the contents**, because the
	// contents are whatever `scene` has registered in this process and
	// `replication` does not link `scene` — the property that keeps `net` and
	// `replication` separable is that neither knows what a component *is*. So
	// the real component list is asserted where both are linked, and this pins
	// the rule: everything shared, nothing local, and nothing from anywhere
	// else.
	for (const auto &component : DefaultReplicatedComponents()) {
		INFO("component: " << component.Name);

		// Nothing local ever appears, whatever is registered.
		CHECK_FALSE(LocalToTheClient(component.Name));

		// And nothing that is neither a scene component nor the tree.
		const bool shared = component.Name.starts_with("scene.") || component.Name == "ecs.Hierarchy";
		CHECK(shared);
	}
}

TEST_CASE("only the two written every tick are observed", "[replication][defaults]") {
	// Observed: written every tick by a system, so the dirty bits already know
	// and hashing would be a pass over the world to learn what was free.
	//
	// Signed: written once by a script and then never. Observing those buys a
	// dirty column paid every tick and read never — and *not* signing them is
	// the v0.7 bug where a part recoloured at runtime kept its old colour on
	// every client for ever.
	//
	// Asserted as "no third one" rather than as a list, because the list depends
	// on what this process registered and the *rule* does not.
	for (const auto &component : DefaultReplicatedComponents()) {
		if (component.Detection != ChangeDetection::Observed) {
			continue;
		}
		INFO("component: " << component.Name);
		CHECK((component.Name == "scene.Transform" || component.Name == "scene.Motion"));
	}
}

TEST_CASE("nothing is declared twice and every name is qualified", "[replication][defaults]") {
	// A duplicate row is two answers to how a component's changes are found, and
	// `Authority::Replicate` takes the last one — a detector chosen by list
	// order, which is the silent kind of wrong this table exists to remove.
	const auto table = DefaultReplicatedComponents();
	CHECK_FALSE(table.empty());

	for (size_t outer = 0; outer < table.size(); outer++) {
		INFO("component: " << table[outer].Name);

		// An unqualified name is one no store resolves — a row that declares
		// nothing and reports nothing.
		CHECK(table[outer].Name.find('.') != std::string_view::npos);

		for (size_t inner = outer + 1; inner < table.size(); inner++) {
			CHECK(table[outer].Name != table[inner].Name);
		}
	}
}
