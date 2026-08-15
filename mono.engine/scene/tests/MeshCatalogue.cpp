// The catalogue behind `MeshPart.TrianglesCount`.
//
// Two things here fail silently if they are wrong, and they are what these
// pin. A read-only property that is quietly writable is a script able to lie
// about content it does not own; and a getter that *acquires* the resource
// mutates the world from inside a read, which is a structural change during
// iteration on the first frame and on every part in the scene.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

TEST_SUITE_ID("engine.scene.meshcatalogue")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::MeshCatalogue;
using engine::scene::MeshesOf;
using engine::scene::RecordMesh;
using engine::scene::TrianglesOf;
using engine::scene::Visual;

namespace {
	// **Registered before the store exists, and every case here goes through
	// this.** `MeshesOf` sets a resource, `SetResource` keys on a component id,
	// and an id minted before the explicit registration lands takes the
	// compiler's spelling of the type - which aborts the process the moment
	// `RegisterSceneComponents` gets there.
	//
	// The abort is order-dependent, so a suite that gets this wrong passes most
	// runs. That is how it was found here: nine green runs and a tenth that
	// aborted inside an unrelated `ActiveCamera` case, because that run
	// happened to schedule a catalogue case first.
	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	Entity MadeMeshPart(Store &store, const char *name) {
		engine::scene::RegisterSceneClasses();
		return store.CreateInstance(engine::ecs::Classes::Find(Name("MeshPart")), name);
	}

	int32_t CountOf(const Store &store, Entity instance) {
		int32_t value = -1;
		REQUIRE(store.GetProperty(instance, Name("TrianglesCount"), &value, sizeof(value)));
		return value;
	}
}

TEST_CASE("a recorded mesh reports its triangles", "[scene][meshcatalogue]") {
	Store store = Fresh("mesh_catalogue_test.record");

	REQUIRE(RecordMesh(store, Name("catalogue_test/fox.amesh"), 17052));
	CHECK(TrianglesOf(store, Name("catalogue_test/fox.amesh")) == 17052);

	// Re-registering replaces rather than refuses: a publisher may put new
	// geometry under a name it already used, and a catalogue that kept the
	// first would answer with geometry nothing is drawing.
	REQUIRE(RecordMesh(store, Name("catalogue_test/fox.amesh"), 9));
	CHECK(TrianglesOf(store, Name("catalogue_test/fox.amesh")) == 9);
}

TEST_CASE("an unknown mesh is zero rather than a guess", "[scene][meshcatalogue]") {
	Store store = Fresh("mesh_catalogue_test.unknown");
	REQUIRE(RecordMesh(store, Name("catalogue_test/known.amesh"), 4));

	CHECK(TrianglesOf(store, Name("catalogue_test/never.amesh")) == 0);

	// An invalid name is the case a part with no `MeshId` produces, and it has
	// to be the same answer rather than a lookup on a null id.
	CHECK(TrianglesOf(store, Name()) == 0);
	CHECK_FALSE(RecordMesh(store, Name(), 4));
}

TEST_CASE("reading a count never creates the resource", "[scene][meshcatalogue]") {
	Store store = Fresh("mesh_catalogue_test.read_only_resource");

	// **This is the property getter's path**, and it runs on every mesh part
	// in the scene during `PreRender`. Acquiring the resource here would be a
	// structural write from inside a read.
	CHECK(TrianglesOf(store, Name("catalogue_test/fox.amesh")) == 0);
	CHECK_FALSE(store.HasResource<MeshCatalogue>());

	// The acquiring form is the one that may, and it is what `RecordMesh` uses.
	MeshesOf(store);
	CHECK(store.HasResource<MeshCatalogue>());
}

TEST_CASE("TrianglesCount reads the mesh the part names", "[scene][meshcatalogue]") {
	Store store = Fresh("mesh_catalogue_test.property");

	const Entity part = MadeMeshPart(store, "Fox");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// Before anything is recorded, and before a `MeshId` is even set. Zero is
	// "this world has not been told", which is the honest answer on a headless
	// server and on a client whose content has not arrived.
	CHECK(CountOf(store, part) == 0);

	const Name fox("catalogue_test/fox.amesh");
	REQUIRE(store.SetProperty(part, Name("MeshId"), &fox, sizeof(Name)));

	// Named and still nothing recorded: a `MeshId` that resolves to no
	// published mesh is exactly the condition that makes the part draw as
	// `MeshTable`'s fallback cube, and it reads as zero here rather than as a
	// cube's twelve.
	CHECK(CountOf(store, part) == 0);

	REQUIRE(RecordMesh(store, Name("catalogue_test/fox.amesh"), 17052));
	CHECK(CountOf(store, part) == 17052);

	// **Follows the name rather than caching it**, which is the whole reason
	// the count lives in a catalogue instead of on the row: repointing
	// `MeshId` has to change the answer, and a copy written when the mesh was
	// assigned would still be reporting the fox.
	REQUIRE(RecordMesh(store, Name("catalogue_test/dragon.amesh"), 87666));

	const Name dragon("catalogue_test/dragon.amesh");
	REQUIRE(store.SetProperty(part, Name("MeshId"), &dragon, sizeof(Name)));
	CHECK(CountOf(store, part) == 87666);
}

TEST_CASE("TrianglesCount refuses a write", "[scene][meshcatalogue]") {
	Store store = Fresh("mesh_catalogue_test.readonly");

	const Entity part = MadeMeshPart(store, "Fox");
	const Name fox("catalogue_test/fox.amesh");
	REQUIRE(store.SetProperty(part, Name("MeshId"), &fox, sizeof(Name)));
	REQUIRE(RecordMesh(store, fox, 17052));

	// **The refusal is the feature.** A part's other properties are facts about
	// the part, which its author owns; this is a fact about the mesh, which a
	// publisher owns. A script that could assign it would be claiming a fox is
	// eight triangles, with nowhere for the write to go and nothing downstream
	// any the wiser.
	const int32_t lie = 8;
	CHECK_FALSE(store.SetProperty(part, Name("TrianglesCount"), &lie, sizeof(lie)));
	CHECK(CountOf(store, part) == 17052);

	// And the descriptor says so, so a properties panel and both script
	// bindings refuse it from the same flag rather than each having a rule.
	bool found = false;
	for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(part)) {
		if (property.Name == Name("TrianglesCount")) {
			found = true;
			CHECK_FALSE(property.Writable);
			CHECK(property.Set == nullptr);
			CHECK(property.Type == engine::ecs::PropertyType::Int32);
		}
	}
	CHECK(found);
}

TEST_CASE("a catalogue is not carried by a save file", "[scene][meshcatalogue]") {
	Store store = Fresh("mesh_catalogue_test.derived");
	REQUIRE(RecordMesh(store, Name("catalogue_test/fox.amesh"), 17052));

	engine::core::ByteWriter writer;
	REQUIRE(store.Save(writer));

	Store restored = Fresh("mesh_catalogue_test.derived");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	// **Derived state, so it comes back empty rather than stale.** The counts
	// come from whatever registered the meshes this run; a file carrying last
	// run's numbers would disagree with the content sitting beside it, and a
	// wrong count is worse than the zero that says "not known here".
	CHECK(TrianglesOf(restored, Name("catalogue_test/fox.amesh")) == 0);
}

TEST_CASE("a mesh reports the sheets its submeshes name", "[scene][meshcatalogue]") {
	// **What a model is wearing, which nothing else could answer.** The names
	// live inside the mesh file, so an author who wants to swap a character's
	// outfit had no way to learn the current sheet and no name to put back.
	Store store = Fresh("mesh_catalogue_test.sheets");

	const Name fox("catalogue_test/fox.amesh");
	const std::array<Name, 3> worn{Name("skins/body.atex"), Name("skins/eyes.atex"), Name("skins/body.atex")};

	REQUIRE(RecordMesh(store, fox, 17052, worn));

	std::vector<Name> read;
	CHECK(engine::scene::SheetsOf(store, fox, read) == 3);
	REQUIRE(read.size() == 3);

	// **In submesh order with duplicates kept**, unlike every other list this
	// module hands out. A character with twenty runs sharing four sheets is four
	// names repeated, and which run wears which is a fact - collapsing it here
	// would lose it for good.
	CHECK(read[0] == Name("skins/body.atex"));
	CHECK(read[1] == Name("skins/eyes.atex"));
	CHECK(read[2] == Name("skins/body.atex"));

	// **Replaced rather than merged**, for the same reason the count is: a
	// republished mesh may name different sheets, and a leftover would be a name
	// somebody puts back onto geometry that no longer wears it - worse than not
	// knowing, because it looks like an answer.
	REQUIRE(RecordMesh(store, fox, 9, std::array<Name, 1>{Name("skins/plain.atex")}));
	CHECK(engine::scene::SheetsOf(store, fox, read) == 1);
	REQUIRE(read.size() == 1);
	CHECK(read[0] == Name("skins/plain.atex"));

	// A mesh recorded with none - every built-in is one - reads back empty
	// rather than keeping what it had.
	REQUIRE(RecordMesh(store, fox, 12));
	CHECK(engine::scene::SheetsOf(store, fox, read) == 0);
	CHECK(read.empty());
}

TEST_CASE("sheets for an unknown mesh are empty, and asking creates nothing", "[scene][meshcatalogue]") {
	Store store = Fresh("mesh_catalogue_test.sheets_unknown");

	// **The reader's path**, and it must not acquire the resource - the same
	// rule `TrianglesOf` keeps, for the same reason: it is what a script binding
	// calls, and a read that mutated the world would be a structural write from
	// inside one.
	std::vector<Name> read{Name("stale.atex")};
	CHECK(engine::scene::SheetsOf(store, Name("catalogue_test/never.amesh"), read) == 0);

	// Cleared, so a caller reusing a vector cannot read the previous answer as
	// this one.
	CHECK(read.empty());
	CHECK_FALSE(store.HasResource<engine::scene::MeshCatalogue>());

	// An invalid name is what a part with no `MeshId` produces, and it is the
	// same answer rather than a lookup on a null id.
	CHECK(engine::scene::SheetsOf(store, Name(), read) == 0);
}
