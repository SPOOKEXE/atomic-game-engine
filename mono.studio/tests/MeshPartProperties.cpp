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
