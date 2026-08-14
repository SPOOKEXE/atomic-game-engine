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

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.replication.defaults")

using engine::replication::ChangeDetection;
using engine::replication::DefaultReplicatedComponents;
using engine::replication::LocalToTheClient;

namespace defaults_test {
	// A component that should cross: trivially copyable, with a serialisation,
	// under the shared prefix.
	struct Shared {
		float Value = 0.0f;
	};

	// One that should not, and for a reason the derivation checks rather than
	// guesses: everything but the two tick-written components is signature
	// detected, and a signature is a hash of the bytes — which a type holding a
	// `std::string` does not have. The three scene catalogues are exactly this
	// shape and were a warning per host per run before the filter existed.
	struct Bulky {
		std::string Text;
	};

	// And one under nobody's prefix, which is the "nothing from anywhere else"
	// half of the rule.
	struct Outside {
		int Value = 0;
	};

	// **Registered by this suite, which is what makes the cases below say
	// anything.** The table is a function-local static built on first use, so in
	// a host it is built after start-up registered everything and here it is
	// built by whichever case runs first — which is why every case calls this
	// and why the names are this suite's own.
	//
	// They are `scene.`-prefixed because the prefix is the rule under test.
	// Nothing in this binary registers a real `scene.` name — `replication` does
	// not link `scene` and must not — so there is no collision to have. An
	// earlier version of this suite registered stand-ins under the *real* names
	// and aborted the binary in suite order, which is the mistake these
	// spellings avoid rather than a style preference.
	void Ready() {
		engine::ecs::Components::Register<Shared>(
			"scene.DefaultsTestShared",
			[](engine::core::ByteWriter &writer, const void *values, size_t count) {
				const auto *shared = static_cast<const Shared *>(values);
				for (size_t index = 0; index < count; index++) {
					writer.WriteFloat(shared[index].Value);
				}
			},
			[](engine::core::ByteReader &reader, void *values, size_t count) {
				auto *shared = static_cast<Shared *>(values);
				for (size_t index = 0; index < count; index++) {
					shared[index].Value = reader.ReadFloat();
				}
			}
		);
		engine::ecs::Components::Register<Bulky>("scene.DefaultsTestBulky");
		engine::ecs::Components::Register<Outside>("defaults_test.Outside");

		// **The `ecs.` half of the table, and it has to be registered before the
		// table is built or the exhaustiveness case below asserts over nothing.**
		// `RegisterInstanceRoot` is idempotent and is the public way in — it
		// registers `ecs.Hierarchy`, `ecs.InstanceName`, `ecs.InstanceClass` and
		// `ecs.AttributeTable`, which is the whole set the case has to classify.
		engine::ecs::Classes::RegisterInstanceRoot();
	}
}

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
	defaults_test::Ready();

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

	// **And the same line drawn one step further on.** A `SurfaceCamera` is
	// authored scene content and crosses; the frustum *fitted to its pane* is
	// made from where the local eye is standing, so the authority's answer is
	// correct for the authority's camera and wrong for every client watching.
	// Both ends run `AimSurfaceCameras` and recompute it — what crosses is the
	// mirror, never the aim.
	CHECK(LocalToTheClient("scene.SurfaceLens"));

	// Which part a portal leads to is a fact about the scene rather than about
	// the viewer, so it crosses like any other authored thing. A client that had
	// to work it out for itself could not: there is nothing local to derive it
	// from.
	CHECK_FALSE(LocalToTheClient("scene.Portal"));
}

TEST_CASE("what a machine works out for itself is not sent", "[replication][defaults]") {
	defaults_test::Ready();

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

TEST_CASE("the derivation keeps what can cross and drops what cannot", "[replication][defaults]") {
	defaults_test::Ready();

	// **The positive case, and the suite had none.** Every other case here is a
	// loop over the table, so all of them passed on an empty one — which is
	// exactly what happened: `replication` links no module that registers a
	// `scene.` component, so in this binary the table was empty and four cases
	// agreed about nothing. A derivation that finds nothing is the failure this
	// whole entry was about, so it is asserted directly.
	const auto *shared = Row("scene.DefaultsTestShared");
	REQUIRE(shared != nullptr);

	// Signature, not observed. Only the two components a system writes every
	// tick are observed; anything a script writes once is signed, which is the
	// v0.7 bug where a part recoloured at runtime kept its old colour for ever.
	CHECK(shared->Detection == ChangeDetection::Signature);

	// Skipped for want of a hash rather than for want of a serialiser — the two
	// are different reasons and this type has the second and not the first.
	CHECK(Row("scene.DefaultsTestBulky") == nullptr);

	// And the prefix is a rule rather than a convention: a component this
	// process registered under another module's name is not a world's state.
	CHECK(Row("defaults_test.Outside") == nullptr);
}

TEST_CASE("the set is derived, and never contradicts the exclusions", "[replication][defaults]") {
	defaults_test::Ready();

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

		// And nothing that is neither a scene component nor part of an
		// instance's identity.
		const bool shared = component.Name.starts_with("scene.") || component.Name == "ecs.Hierarchy" ||
							component.Name == "ecs.InstanceName" || component.Name == "ecs.InstanceClass";
		CHECK(shared);
	}
}

TEST_CASE("what an instance is crosses whole", "[replication][defaults]") {
	defaults_test::Ready();

	// **The three together, because two of them went missing for eight versions
	// and nothing said so.** `ecs.Hierarchy` was admitted by name and
	// `ecs.InstanceName` and `ecs.InstanceClass` were not, so an entity in the
	// join snapshot — `Store::Save` carries every component — arrived named and
	// an entity created afterwards arrived with no name and no class. A tree
	// position without a name or a class is a node a client can hold and cannot
	// ask for.
	REQUIRE(Row("ecs.Hierarchy") != nullptr);
	REQUIRE(Row("ecs.InstanceName") != nullptr);
	REQUIRE(Row("ecs.InstanceClass") != nullptr);

	// Signed, not observed. A name and a class are set at creation and almost
	// never again, so the dirty column an observed component costs every tick
	// would be paid for a write that does not happen — and *almost* never is why
	// they are not simply said once at creation instead. See `Defaults.cpp`.
	CHECK(Row("ecs.InstanceName")->Detection == ChangeDetection::Signature);
	CHECK(Row("ecs.InstanceClass")->Detection == ChangeDetection::Signature);
}

TEST_CASE("every ecs component is classified rather than left to a prefix", "[replication][defaults]") {
	defaults_test::Ready();

	// **The check that would have caught the bug the case above describes, and
	// `AGENTS.md` rule 6 for this table.** The old rule was a `scene.` prefix
	// with one hand-written exception — "or `ecs.Hierarchy`" — and a rule of that
	// shape cannot fail: a component added to `ecs` simply does not appear, on
	// the wire or in this suite.
	//
	// So every registered `ecs.` component has to be *named* on one side or the
	// other. Adding a fourth without deciding is a red suite here rather than a
	// silence on a delta.
	for (size_t index = 0; index < engine::ecs::Components::Count(); index++) {
		const engine::ecs::TypeDescriptor &type =
			engine::ecs::Components::Describe(engine::ecs::ComponentId{static_cast<uint32_t>(index)});

		const std::string_view name = type.Name.Text();
		if (!name.starts_with("ecs.")) {
			continue;
		}

		INFO("component: " << name);

		// `ecs.AttributeTable` is the one deliberately left out, and for the
		// reason the catalogues are: it holds a map, so it is not trivially
		// copyable and cannot be *signed*. A non-trivial component that should
		// cross needs `Observed` and a matching `Store::Observe`, which is a
		// decision per component and belongs in the host that wants it.
		const bool excluded = name == "ecs.AttributeTable";
		CHECK((excluded || Row(name) != nullptr));
	}
}

TEST_CASE("only the two written every tick are observed", "[replication][defaults]") {
	defaults_test::Ready();

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
	defaults_test::Ready();

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
