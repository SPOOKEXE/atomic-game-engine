// The multi-selection property grid without Dear ImGui.

#include <engine/ecs/Classes.hpp>
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

TEST_CASE("mixed classes contribute a root-first union with mixed values", "[studio][properties]") {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	Store store("property_union");
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
	REQUIRE(basePart != nullptr);
	REQUIRE(lightGroup != nullptr);
	CHECK(instance->Applicable == 2);
	CHECK(basePart->Applicable == 1);
	CHECK(lightGroup->Applicable == 1);

	const SelectionPropertyRow *name = Row(*instance, "Name");
	REQUIRE(name != nullptr);
	CHECK(name->Applicable == 2);
	CHECK(name->Readable == 2);
	CHECK(name->Mixed);

	const SelectionPropertyRow *transparency = Row(*basePart, "Transparency");
	const SelectionPropertyRow *brightness = Row(*lightGroup, "Brightness");
	REQUIRE(transparency != nullptr);
	REQUIRE(brightness != nullptr);
	CHECK_FALSE(transparency->Mixed);
	CHECK_FALSE(brightness->Mixed);

	CHECK(
		studio::SelectionPropertyApplies(
			store.ClassOf(part), basePart->Owner, transparency->Descriptor->Name, PropertyType::Float
		)
	);
	CHECK_FALSE(
		studio::SelectionPropertyApplies(
			store.ClassOf(light), basePart->Owner, transparency->Descriptor->Name, PropertyType::Float
		)
	);
	CHECK(
		studio::SelectionPropertyApplies(
			store.ClassOf(light), lightGroup->Owner, brightness->Descriptor->Name, PropertyType::Float
		)
	);
	CHECK_FALSE(
		studio::SelectionPropertyApplies(
			store.ClassOf(part), lightGroup->Owner, brightness->Descriptor->Name, PropertyType::Float
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
