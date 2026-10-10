// The multi-selection property grid without Dear ImGui.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <studio/PropertySelection.hpp>
#include <vector>

TEST_SUITE_ID("studio.propertyselection")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::PropertyType;
using engine::ecs::Store;
using studio::SelectionPropertyGroup;
using studio::SelectionPropertyRow;

namespace {
	const SelectionPropertyGroup *
	Group(const std::vector<SelectionPropertyGroup> &groups, std::string_view owner) {
		const Name wanted(owner);
		const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto &group) {
			return Classes::Describe(group.Owner).Name == wanted;
		});
		return found == groups.end() ? nullptr : &*found;
	}

	const SelectionPropertyRow *Row(const SelectionPropertyGroup &group, std::string_view property) {
		const Name wanted(property);
		const auto found = std::find_if(group.Rows.begin(), group.Rows.end(), [&](const auto &row) {
			return row.Descriptor != nullptr && row.Descriptor->Name == wanted;
		});
		return found == group.Rows.end() ? nullptr : &*found;
	}
}

TEST_CASE("mixed classes expose only their shared property surface", "[studio][properties]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("property_intersection");
	const Entity part = store.CreateInstance(Classes::Find(Name("Part")), "Block");
	const Entity light = store.CreateInstance(Classes::Find(Name("PointLight")), "Lamp");
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	REQUIRE(light != engine::ecs::NULL_ENTITY);

	const std::array selected{part, light};
	const std::vector<SelectionPropertyGroup> groups = studio::BuildPropertySelection(store, selected);

	const SelectionPropertyGroup *instance = Group(groups, "Instance");
	const SelectionPropertyGroup *basePart = Group(groups, "BasePart");
	const SelectionPropertyGroup *lightGroup = Group(groups, "Light");
	REQUIRE(instance != nullptr);
	CHECK(basePart == nullptr);
	CHECK(lightGroup == nullptr);
	CHECK(instance->Applicable == 2);

	const SelectionPropertyRow *name = Row(*instance, "Name");
	REQUIRE(name != nullptr);
	CHECK(name->Applicable == 2);
	CHECK(name->Readable == 2);
	CHECK(name->Mixed);

	CHECK(Row(*instance, "Transparency") == nullptr);
	CHECK(Row(*instance, "Brightness") == nullptr);

	const auto basePartClass = Classes::Find(Name("BasePart"));
	const auto lightClass = Classes::Find(Name("Light"));
	CHECK(
		studio::SelectionPropertyApplies(
			store.ClassOf(part), basePartClass, Name("Transparency"), PropertyType::Float
		)
	);
	CHECK_FALSE(
		studio::SelectionPropertyApplies(
			store.ClassOf(light), basePartClass, Name("Transparency"), PropertyType::Float
		)
	);
	CHECK(
		studio::SelectionPropertyApplies(
			store.ClassOf(light), lightClass, Name("Brightness"), PropertyType::Float
		)
	);
	CHECK_FALSE(
		studio::SelectionPropertyApplies(
			store.ClassOf(part), lightClass, Name("Brightness"), PropertyType::Float
		)
	);
}

TEST_CASE("equal values stay concrete in a multi-selection", "[studio][properties]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("property_agreement");
	const Entity first = store.CreateInstance(Classes::Find(Name("Part")), "First");
	const Entity second = store.CreateInstance(Classes::Find(Name("Part")), "Second");
	const std::array selected{first, second};
	const std::vector<SelectionPropertyGroup> groups = studio::BuildPropertySelection(store, selected);

	const SelectionPropertyGroup *basePart = Group(groups, "BasePart");
	REQUIRE(basePart != nullptr);
	const SelectionPropertyRow *transparency = Row(*basePart, "Transparency");
	REQUIRE(transparency != nullptr);
	CHECK(transparency->Applicable == 2);
	CHECK(transparency->Readable == 2);
	CHECK_FALSE(transparency->Mixed);
}

TEST_CASE("a stale first handle does not hide later live selected properties", "[studio][properties]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("property_stale_selection");
	const Entity stale = store.CreateInstance(Classes::Find(Name("Part")), "Stale");
	const Entity first = store.CreateInstance(Classes::Find(Name("Part")), "First");
	const Entity second = store.CreateInstance(Classes::Find(Name("Part")), "Second");
	store.Destroy(stale);

	const std::array selected{stale, first, second};
	const std::vector<SelectionPropertyGroup> groups = studio::BuildPropertySelection(store, selected);

	const SelectionPropertyGroup *basePart = Group(groups, "BasePart");
	REQUIRE(basePart != nullptr);
	CHECK(basePart->Applicable == 2);
	const SelectionPropertyRow *transparency = Row(*basePart, "Transparency");
	REQUIRE(transparency != nullptr);
	CHECK(transparency->Applicable == 2);
	CHECK(transparency->Readable == 2);
}

TEST_CASE(
	"inspector metadata groups inherited rows without losing their declaring owners",
	"[studio][properties][groups]"
) {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Store store("property_tag_groups");
	const Entity part = store.CreateInstance(Classes::Find(Name("MeshPart")), "Mesh");
	const std::array selection{part};
	const auto original = studio::BuildPropertySelection(store, selection);
	const auto tagged = studio::BuildTaggedPropertySelection(store, selection);
	size_t rowCount = 0;
	for (const auto &group : tagged) {
		REQUIRE(group.PropertiesTag.IsValid());
		for (const auto &row : group.Rows) {
			REQUIRE(row.Descriptor != nullptr);
			CHECK(
				group.PropertiesTag ==
				(row.Descriptor->PropertiesTag.IsValid() ? row.Descriptor->PropertiesTag : Name("Unassigned"))
			);
			CHECK(row.Owner == studio::DeclaringPropertyClass(store.ClassOf(part), row.Descriptor->Name));
			CHECK(
				studio::SelectionPropertyApplies(
					store.ClassOf(part), row.Owner, row.Descriptor->Name, row.Descriptor->Type
				)
			);
			rowCount++;
		}
	}
	size_t originalCount = 0;
	for (const auto &group : original)
		originalCount += group.Rows.size();
	CHECK(rowCount == originalCount);
}

TEST_CASE(
	"properties with no inspector metadata remain visible as Unassigned", "[studio][properties][groups]"
) {
	struct Untagged {
		float Amount = 0;
	};
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	const auto component = engine::ecs::Components::Register<Untagged>("studio.test.UntaggedProperty");
	const std::array components{component};
	const auto owner =
		Classes::Register("StudioUntaggedProperty", Classes::Find(Name("Instance")), components);
	Classes::Property<&Untagged::Amount>(owner, "UnmappedAmount");
	Store store("untagged_properties");
	const Entity entity = store.CreateInstance(owner, "Untyped");
	const std::array selection{entity};
	const auto groups = studio::BuildTaggedPropertySelection(store, selection);
	const auto found = std::find_if(groups.begin(), groups.end(), [](const auto &group) {
		return group.PropertiesTag == Name("Unassigned");
	});
	REQUIRE(found != groups.end());
	const auto *row = Row(*found, "UnmappedAmount");
	REQUIRE(row != nullptr);
	CHECK(row->Owner == owner);
	CHECK(row->Readable == 1);
	CHECK(row->Value.Float == 0);
}
