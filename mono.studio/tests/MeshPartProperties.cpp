// What the properties panel decides about a `MeshPart`'s content rows.
//
// **Three independent things have to agree before a picker button appears**, and
// none of them is in the panel's drawing code:
//
//   * `game::ReadProperty` has to answer — a row it cannot read is drawn as `—`
//     with no field and no button at all;
//   * `PropertyDescriptor::Writable` has to be true — the row is inside a
//     `BeginDisabled(!Writable)` and the button is `&& !locked`;
//   * `studio::ContentKindOfProperty` has to recognise the spelling — a miss is
//     a plain text field, which is the "there is no picker" symptom exactly.
//
// A failure in any one of them looks identical from the outside: you click and
// nothing happens. This suite is here so the three can be told apart without a
// window, because the panel that joins them needs one and this does not.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <studio/Assets.hpp>

TEST_SUITE_ID("studio.meshpartproperties")

using engine::assets::AssetKind;
using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::PropertyDescriptor;
using engine::ecs::Store;
using studio::ContentKindOfProperty;

namespace {
	// The panel's own three questions, asked of one property.
	struct Row {
		bool Found = false;
		bool Readable = false;
		bool Writable = false;
		AssetKind Kind = AssetKind::Unknown;
	};

	Row Inspect(Store &store, Entity instance, std::string_view property) {
		Row row;
		const engine::ecs::ClassId klass = store.ClassOf(instance);
		if (!klass.IsValid()) {
			return row;
		}

		for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
			if (descriptor.Spelling != property) {
				continue;
			}
			row.Found = true;
			row.Writable = descriptor.Writable;

			engine::game::PropertyValue value;
			row.Readable = engine::game::ReadProperty(store, instance, descriptor, value);

			// **`Spelling` and not `Name.Text()`**, because that is what the
			// panel passes — and if the two ever stopped agreeing, the picker
			// would vanish from every content property at once.
			row.Kind = ContentKindOfProperty(descriptor.Spelling);
			return row;
		}
		return row;
	}
}

TEST_CASE("a mesh part's content rows all get a picker", "[studio][properties]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("meshpart_properties");
	const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Imported");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// `MeshId` and `TextureID` are Roblox's names and, since v0.10, the only
	// ones — the `BasePart` aliases `Mesh` and `ColorMap` are gone, because
	// geometry loaded from a file is not something a plain `Part` has.
	for (const auto &[property, kind] : {
			 std::pair<std::string_view, AssetKind>{"MeshId", AssetKind::Mesh},
			 std::pair<std::string_view, AssetKind>{"TextureID", AssetKind::Texture},
		 }) {
		INFO(property);
		const Row row = Inspect(store, part, property);

		// Declared at all. A property removed from the class tree takes its
		// picker with it and the row simply is not there.
		CHECK(row.Found);

		// Readable, or the panel draws `—` and returns before any widget.
		CHECK(row.Readable);

		// Writable, or the row is disabled and the `...` button is `&& !locked`.
		CHECK(row.Writable);

		// Recognised, or the row is a bare text field.
		CHECK(row.Kind == kind);
	}
}

TEST_CASE("a plain part offers neither row", "[studio][properties]") {
	// **The point of the split, from the panel's side.** A mesh reference on a
	// `Part` was a picker that did nothing: you chose a mesh, the six-sided box
	// did not change, and nothing said the class was the wrong one. A `Part` is
	// textured by a `Material` instance under it instead.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("part_properties");
	const Entity part = store.CreateInstance(Classes::Find(Name("Part")), "Plain");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	CHECK_FALSE(Inspect(store, part, "Mesh").Found);
	CHECK_FALSE(Inspect(store, part, "MeshId").Found);
	CHECK_FALSE(Inspect(store, part, "ColorMap").Found);
	CHECK_FALSE(Inspect(store, part, "TextureID").Found);
}

// --- what the picker hands back ---------------------------------------------
//
// **A fourth question, and it is the one that was wrong.** The three above ask
// whether the row *offers* a picker. This asks whether confirming one writes
// anything, and for five properties across four classes the answer was no: the
// panel built a `PropertyValue` from nothing, set `Name` on it, and left `Type`
// at its default of `Opaque` — which `game::WriteProperty` refuses on its first
// line, before it has looked at the store.
//
// Nothing reported it. `WriteProperty` returns `false` and the caller uses that
// only to decide whether to record an undo entry, so a refused write and a write
// that changed nothing are the same non-event. The symptom was a `MeshId` that
// stayed blank, a `MeshPart` that kept drawing `MeshTable`'s fallback cube, and a
// `TextureID` that never tinted it — three bugs with one cause, and all three
// looked like content that had failed to arrive.
// **The tests call `studio::ChosenContentValue`, which is the function the panel
// calls.** A helper written out here instead would be a second copy of the thing
// under test, and it would have passed on the day the panel was broken.
namespace {
	// Finds a descriptor by the spelling the panel shows.
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

TEST_CASE("confirming a picker writes the property", "[studio][properties]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("picker_writes");
	const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Imported");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	const PropertyDescriptor *mesh = DescriptorFor(store, part, "MeshId");
	REQUIRE(mesh != nullptr);

	// The write itself. **This is the assertion the editor was missing**: it
	// returned `false` and the panel carried on as though a property had been
	// set.
	CHECK(engine::game::WriteProperty(store, part, *mesh, studio::ChosenContentValue(mesh->Type, "props/fox.amesh")));

	// And it landed where the renderer reads it, not merely somewhere.
	// `client::CollectInstances` copies `Visual::Mesh` into the draw list, so a
	// write that stopped short of this component is a part that keeps drawing
	// the fallback however good the string looked in the panel.
	const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(part);
	REQUIRE(visual != nullptr);
	CHECK(visual->Mesh == Name("props/fox.amesh"));
}

TEST_CASE("a picker value with no type is refused", "[studio][properties]") {
	// **The bug itself, pinned rather than described.** This is what the panel
	// used to send, and it must keep being refused — `WriteProperty`'s type check
	// is what stops a `Name` landing in a `float` — so the fix belongs in the
	// caller and this case exists to stop somebody "fixing" it by loosening the
	// check instead.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("picker_untyped");
	const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Imported");
	const PropertyDescriptor *mesh = DescriptorFor(store, part, "MeshId");
	REQUIRE(mesh != nullptr);

	engine::game::PropertyValue untyped;
	untyped.Name = Name("props/fox.amesh");
	CHECK(untyped.Type == engine::ecs::PropertyType::Opaque);
	CHECK_FALSE(engine::game::WriteProperty(store, part, *mesh, untyped));

	const engine::scene::Visual *visual = store.Get<engine::scene::Visual>(part);
	REQUIRE(visual != nullptr);
	CHECK_FALSE(visual->Mesh.IsValid());
}

TEST_CASE("every content row the picker serves round-trips", "[studio][properties]") {
	// **All five, because the failure was per-property and looked per-class.**
	// One test on `MeshId` would have passed the day somebody fixed meshes and
	// left `Image` on an `ImageLabel` doing nothing — which is how the aliases
	// this file's first case describes went wrong the first time.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	struct Row {
		std::string_view Class;
		std::string_view Property;
	};

	for (const Row &row : {
			 Row{"MeshPart", "MeshId"},
			 Row{"MeshPart", "TextureID"},
			 Row{"Material", "MaterialId"},
			 Row{"Sound", "SoundId"},
		 }) {
		INFO(row.Class << "." << row.Property);

		Store store("picker_rows");
		const Entity instance = store.CreateInstance(Classes::Find(Name(row.Class)), "Subject");
		REQUIRE(instance != engine::ecs::NULL_ENTITY);

		const PropertyDescriptor *descriptor = DescriptorFor(store, instance, row.Property);
		REQUIRE(descriptor != nullptr);

		// A picker only ever offers a content property, so a row here that is
		// not one is a row this table should not carry.
		REQUIRE(ContentKindOfProperty(descriptor->Spelling) != AssetKind::Unknown);

		CHECK(engine::game::WriteProperty(store, instance, *descriptor, studio::ChosenContentValue(descriptor->Type, "chosen.asset")));

		engine::game::PropertyValue read;
		REQUIRE(engine::game::ReadProperty(store, instance, *descriptor, read));
		CHECK(read.Name == Name("chosen.asset"));

		// **"Clear" is the same path with an empty string**, and it has to reach
		// the store too — a part with no mesh is a plain part, and a Clear that
		// silently did nothing would be indistinguishable from one that worked.
		CHECK(engine::game::WriteProperty(store, instance, *descriptor, studio::ChosenContentValue(descriptor->Type, "")));
		REQUIRE(engine::game::ReadProperty(store, instance, *descriptor, read));
		CHECK_FALSE(read.Name.IsValid());
	}
}

TEST_CASE("a material instance's one row gets a picker", "[studio][properties]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("material_properties");
	const Entity material = store.CreateInstance(Classes::Find(Name("Material")), "Material");
	REQUIRE(material != engine::ecs::NULL_ENTITY);

	const Row row = Inspect(store, material, "MaterialId");
	CHECK(row.Found);
	CHECK(row.Readable);
	CHECK(row.Writable);
	CHECK(row.Kind == AssetKind::Material);
}
