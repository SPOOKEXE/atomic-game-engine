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

	// `MeshId` and `TextureID` are Roblox's names and the ones a `MeshPart`
	// actually shows; `Mesh` and `ColorMap` are the same two components under
	// `BasePart`'s spellings. An alias missing from the table is a plain text
	// field on the name somebody is looking at, which `Assets.hpp` calls the
	// exact cost of keying on the property.
	for (const auto &[property, kind] : {
			 std::pair<std::string_view, AssetKind>{"MeshId", AssetKind::Mesh},
			 std::pair<std::string_view, AssetKind>{"TextureID", AssetKind::Texture},
			 std::pair<std::string_view, AssetKind>{"Mesh", AssetKind::Mesh},
			 std::pair<std::string_view, AssetKind>{"ColorMap", AssetKind::Texture},
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

TEST_CASE("a plain part's content rows do too", "[studio][properties]") {
	// **The same check one class down**, because `MeshPart` adds no component —
	// it is the vocabulary, and `Visual::Mesh` and `SurfaceAppearance::ColourMap`
	// sit on `BasePart`. A `Part` that could not pick a texture would mean the
	// two spellings had drifted apart rather than the alias being wrong.
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("part_properties");
	const Entity part = store.CreateInstance(Classes::Find(Name("Part")), "Plain");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	const Row mesh = Inspect(store, part, "Mesh");
	CHECK(mesh.Found);
	CHECK(mesh.Readable);
	CHECK(mesh.Writable);
	CHECK(mesh.Kind == AssetKind::Mesh);

	const Row colour = Inspect(store, part, "ColorMap");
	CHECK(colour.Found);
	CHECK(colour.Readable);
	CHECK(colour.Writable);
	CHECK(colour.Kind == AssetKind::Texture);
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
