// Resolving a mesh from the id somebody wrote in `MeshId`.
//
// **There is one id space and four things read it**, which is the property this
// suite exists to hold. A `MeshPart.MeshId` is a string — rule 4, a name crosses
// and a number does not — and between an author typing it and a triangle
// appearing, four separate pieces of code have to agree about what that string
// means:
//
//   * `AssetPicker` decides whether to *offer* it,
//   * `Editor::RequestContentAsset` decides whether to *fetch* it,
//   * `MeshPreview` decides how to *load* it for a picture,
//   * `render::MeshTable::Resolve` decides what to *draw*.
//
// They agree today by all asking `assets::BuiltinFromName` the same question.
// The failure when they do not is silent in both directions: a built-in fetched
// from a CDN is a miss logged as though content were broken, and a store asset
// handed to `MakeBuiltin` is a cube where a dragon should be — and
// `MeshTable::Resolve` returns the fallback cube for an unknown name, which is
// also exactly what a mesh that has not arrived yet looks like.
//
// **Every case here runs with no store and no delivery client**, which is the
// point: the six built-in ids are the geometry an editor can always resolve, and
// an engine that needed a published manifest to draw a cube would be one nobody
// could open on a fresh machine.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Builtin.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <studio/Preview.hpp>
#include <studio/Assets.hpp>
#include <vector>

TEST_SUITE_ID("studio.meshresolution")

using engine::assets::BuiltinFromName;
using engine::assets::BuiltinMesh;
using engine::assets::BuiltinName;
using engine::assets::BUILTIN_MESH_COUNT;
using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::PropertyDescriptor;
using engine::ecs::Store;

namespace {
	// Every id the engine resolves without a store.
	std::vector<std::string> BuiltinIds() {
		std::vector<std::string> ids;
		ids.reserve(BUILTIN_MESH_COUNT);
		for (uint8_t index = 0; index < BUILTIN_MESH_COUNT; index++) {
			ids.emplace_back(BuiltinName(static_cast<BuiltinMesh>(index)));
		}
		return ids;
	}

	const PropertyDescriptor *DescriptorFor(Store &store, Entity instance, std::string_view property) {
		const engine::ecs::ClassId klass = store.ClassOf(instance);
		if (!klass.IsValid()) {
			return nullptr;
		}
		for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
			if (descriptor.Spelling == property) {
				return &descriptor;
			}
		}
		return nullptr;
	}
}

TEST_CASE("every built-in id resolves to geometry", "[studio][mesh]") {
	// The base case, and the one the others are relative to: an id the engine
	// ships resolves to a mesh with triangles in it, with nothing published and
	// no file on disk.
	for (const std::string &id : BuiltinIds()) {
		INFO(id);

		BuiltinMesh which = BuiltinMesh::Cube;
		REQUIRE(BuiltinFromName(id, which));

		const engine::assets::MeshData mesh = engine::assets::MakeBuiltin(which);
		CHECK(mesh.IsValid());
		CHECK(mesh.Indices.size() % 3 == 0);
		CHECK(mesh.Indices.size() >= 3);
	}
}

TEST_CASE("a built-in id is offered by the picker", "[studio][mesh]") {
	// **`IsRuntimeReadable` is the picker's own filter**, and it works on the
	// extension. A built-in id has no extension in the usual sense —
	// `engine.Cube` ends in `.Cube` — so this pins that the filter does not read
	// that as a source format and drop the only meshes an empty store has.
	for (const std::string &id : BuiltinIds()) {
		INFO(id);
		CHECK(engine::assets::IsRuntimeReadable(id));
	}
}

TEST_CASE("a built-in id is never fetched", "[studio][mesh]") {
	// **The other half of offering them.** `CollectWantedContent` walks
	// `Visual::Mesh` and cannot tell a built-in from a store asset — nor should
	// it, because it is one list of names — so the decision is made where the
	// request is issued. A built-in that reached a delivery client would be a
	// guaranteed miss, logged once per id, describing geometry that is already
	// registered.
	//
	// Asserted through `BuiltinFromName`, which is the test
	// `Editor::RequestContentAsset` makes, rather than through the editor: the
	// editor needs a device and this needs nothing.
	for (const std::string &id : BuiltinIds()) {
		INFO(id);
		BuiltinMesh ignored = BuiltinMesh::Cube;
		CHECK(BuiltinFromName(id, ignored));
	}

	// And a store id must not be mistaken for one, or a published mesh would be
	// silently replaced by a primitive.
	BuiltinMesh ignored = BuiltinMesh::Cube;
	CHECK_FALSE(BuiltinFromName("7d64f772f71528d2a0c60089970422b5f9b938807cc70b49c285f266bed87e5d.amesh", ignored));
	CHECK_FALSE(BuiltinFromName("props/fox.amesh", ignored));
	CHECK_FALSE(BuiltinFromName("", ignored));
}

TEST_CASE("a mesh part keeps the id it is given", "[studio][mesh]") {
	// **The whole point of an id rather than a handle.** It survives a write and
	// a read unchanged, which is what makes it something an author can copy
	// between two parts, paste into a script, or type from a note.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("mesh_ids");
	const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Subject");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	const PropertyDescriptor *mesh = DescriptorFor(store, part, "MeshId");
	REQUIRE(mesh != nullptr);

	for (const std::string &id : BuiltinIds()) {
		INFO(id);
		REQUIRE(engine::game::WriteProperty(store, part, *mesh, studio::ChosenContentValue(mesh->Type, id)));

		engine::game::PropertyValue read;
		REQUIRE(engine::game::ReadProperty(store, part, *mesh, read));
		CHECK(read.Name == Name(id));

		// And it landed on the component the draw list copies from, not merely
		// somewhere a getter could find it again.
		const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(part);
		REQUIRE(visual != nullptr);
		CHECK(visual->Mesh == Name(id));
	}
}

TEST_CASE("two parts naming one id share it", "[studio][mesh]") {
	// **Instancing is what the id buys**, and it costs nothing to state: two
	// parts naming the same string are two `DrawInstance`s carrying the same
	// `core::Name`, which `MeshTable::Resolve` answers from one entry and one
	// upload. Counting how many name an id is therefore counting instances —
	// there is no per-part copy of the geometry to find.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("mesh_sharing");
	const Entity first = store.CreateInstance(Classes::Find(Name("MeshPart")), "First");
	const Entity second = store.CreateInstance(Classes::Find(Name("MeshPart")), "Second");

	const PropertyDescriptor *mesh = DescriptorFor(store, first, "MeshId");
	REQUIRE(mesh != nullptr);

	const std::string id(BuiltinName(BuiltinMesh::Sphere));
	REQUIRE(engine::game::WriteProperty(store, first, *mesh, studio::ChosenContentValue(mesh->Type, id)));
	REQUIRE(engine::game::WriteProperty(store, second, *mesh, studio::ChosenContentValue(mesh->Type, id)));

	// The same interned id, not two equal strings. `core::Name` is the identity
	// the renderer hashes, so this is the assertion that says "one entry".
	CHECK(store.Get<engine::scene::Visual>(first)->Mesh.Id() ==
		  store.Get<engine::scene::Visual>(second)->Mesh.Id());

	// Counting them is a walk over one column, which is what makes "instance
	// them by counting the number we need" a thing a scene can do.
	size_t naming = 0;
	store.Each<engine::scene::Visual>([&naming, &id](Entity, engine::scene::Visual &visual) {
		if (visual.Mesh == Name(id)) {
			naming++;
		}
	});
	CHECK(naming == 2);
}

TEST_CASE("an unknown id is not silently a cube", "[studio][mesh]") {
	// **The honest degraded state, pinned where it can be seen.**
	// `MeshTable::Resolve` draws the fallback cube for a name it does not hold,
	// which is deliberate and is why a wrong id looks exactly like a mesh that
	// has not streamed in. What must stay true is that the *world* keeps the id
	// it was given rather than being rewritten to a built-in — so
	// `TrianglesCount` reads zero and says "this world does not hold that",
	// which is the one signal that tells the two apart.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("mesh_unknown");
	const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Subject");
	const PropertyDescriptor *mesh = DescriptorFor(store, part, "MeshId");
	REQUIRE(mesh != nullptr);

	REQUIRE(engine::game::WriteProperty(
		store, part, *mesh, studio::ChosenContentValue(mesh->Type, "nobody/published/this.amesh")
	));

	CHECK(store.Get<engine::scene::Visual>(part)->Mesh == Name("nobody/published/this.amesh"));
	CHECK(engine::scene::TrianglesOf(store, Name("nobody/published/this.amesh")) == 0);

	// And once the world does hold it, the same id reports what arrived.
	REQUIRE(engine::scene::RecordMesh(store, Name("nobody/published/this.amesh"), 3706));
	CHECK(engine::scene::TrianglesOf(store, Name("nobody/published/this.amesh")) == 3706);
}

TEST_CASE("a material previews as a render, not a bitmap", "[studio][meshresolution]") {
	using engine::assets::AssetKind;

	// **The two kinds that have no picture of their own.** A mesh is geometry
	// and a material is a texture *reference* — neither is bytes anybody can
	// blit, so both go to the preview slot and get drawn.
	CHECK(studio::PreviewIsRendered(AssetKind::Mesh));
	CHECK(studio::PreviewIsRendered(AssetKind::Material));

	// **A texture already is its own picture**, so sending it to the slot would
	// be a camera, a pass and a target to show pixels that were in hand.
	CHECK_FALSE(studio::PreviewIsRendered(AssetKind::Texture));
	CHECK_FALSE(studio::PreviewIsRendered(AssetKind::Audio));
	CHECK_FALSE(studio::PreviewIsRendered(AssetKind::Unknown));
}
