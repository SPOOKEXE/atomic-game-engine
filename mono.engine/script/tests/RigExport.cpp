#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/RigExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.script.rigexport")
TEST_DEPENDS("engine.scene.skinning")

namespace {
	void Identify(engine::ecs::Store &store, engine::ecs::Entity entity, std::string_view id) {
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = id;
		REQUIRE(engine::ecs::SetAttribute(store, entity, engine::core::Name("DataFactoryId"), value));
	}

	const engine::script::ScriptValue *Field(const engine::script::ScriptValue &map, std::string_view name) {
		for (const auto &[key, value] : map.Entries)
			if (key == name) return &value;
		return nullptr;
	}
}

TEST_CASE("rig export preserves skeleton frames and dense stable slots", "[script][rigexport]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.populated");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/hero");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("hero-rig"), 2});
	const auto root = store.CreateInstance(engine::scene::BoneClass(), "Root");
	const auto child = store.CreateInstance(engine::scene::BoneClass(), "Child");
	REQUIRE(store.SetParent(root, rig));
	REQUIRE(store.SetParent(child, rig));
	engine::scene::Bone rootBone;
	rootBone.Joint = 0;
	rootBone.Rest.Position = {1, 2, 3};
	engine::scene::Bone childBone;
	childBone.Joint = 1;
	childBone.ParentJoint = 0;
	childBone.Transform.Position = {4, 5, 6};
	store.Set(root, rootBone);
	store.Set(child, childBone);

	const auto exported = engine::script::GetRigExport(store, "export/one");
	REQUIRE(std::string_view(exported.Status) == "ok");
	CHECK(Field(exported.Value, "schema")->Text == "data-rig/v1");
	CHECK(Field(exported.Value, "tick_seconds_numerator")->Number == 8'947'849);
	CHECK(Field(exported.Value, "tick_seconds_denominator")->Number == 536'870'912);
	const auto &entities = Field(exported.Value, "entities")->Items;
	REQUIRE(entities.size() == 1);
	const auto &joints = Field(entities[0], "joints")->Items;
	REQUIRE(joints.size() == 2);
	CHECK(Field(joints[0], "joint_id")->Text == "rig/hero:joint:0");
	CHECK(Field(joints[1], "parent_joint_id")->Text == "rig/hero:joint:0");
	const auto *rest = Field(joints[0], "rest_frame");
	REQUIRE(rest != nullptr);
	const auto *translation = Field(*rest, "translation");
	REQUIRE(translation != nullptr);
	CHECK(translation->Items[0].Number == 1);
	CHECK(Field(entities[0], "skinning")->Entries[0].second.Boolean == false);
	CHECK(Field(entities[0], "keypoints")->Items.empty());
	CHECK(Field(entities[0], "clips")->Items.empty());
}

TEST_CASE("rig export bounds generated joint IDs and validates rig IDs", "[script][rigexport]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.identifiers");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	const std::string maximum(500, 'a');
	Identify(store, rig, maximum);
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("named"), 1});
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});
	const auto exported = engine::script::GetRigExport(store, "export/ids");
	REQUIRE(std::string_view(exported.Status) == "ok");
	CHECK(
		Field(Field(Field(exported.Value, "entities")->Items[0], "joints")->Items[0], "joint_id")
			->Text.size() <= 512
	);
	engine::ecs::Store tooLong("script.rigexport.long-id");
	const auto tooLongRig = tooLong.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(tooLong, tooLongRig, std::string(501, 'b'));
	tooLong.Set<engine::scene::Skeleton>(tooLongRig, {engine::core::Name("long"), 1});
	CHECK(
		std::string_view(engine::script::GetRigExport(tooLong, "export/long").Status) == "invalid_identity"
	);

	engine::ecs::Store invalid("script.rigexport.invalid-rig");
	const auto invalidRig = invalid.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(invalid, invalidRig, "rig/invalid");
	invalid.Set<engine::scene::Skeleton>(invalidRig, {engine::core::Name(std::string("bad\0rig", 7)), 1});
	const auto invalidBone = invalid.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(invalid.SetParent(invalidBone, invalidRig));
	invalid.Set(invalidBone, engine::scene::Bone{});
	CHECK(
		std::string_view(engine::script::GetRigExport(invalid, "export/invalid").Status) == "invalid_rig_id"
	);
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/capped", {}, 0).Status) ==
		"resource_limit"
	);
}

TEST_CASE("rig export reduces exact stored float tick duration", "[script][rigexport]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.tick");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/tick");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("tick"), 1});
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});
	store.AdvanceTick(std::bit_cast<float>(uint32_t{0x26800000}));
	const auto exported = engine::script::GetRigExport(store, "export/tick");
	REQUIRE(std::string_view(exported.Status) == "ok");
	CHECK(Field(exported.Value, "tick_seconds_numerator")->Number == 1);
	CHECK(Field(exported.Value, "tick_seconds_denominator")->Number == (uint64_t{1} << 50));
}

TEST_CASE(
	"rig export sorts stable entities and refuses unknown selection and sparse bones", "[script][rigexport]"
) {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.refusal");
	for (const char *id : {"rig/z", "rig/a"}) {
		const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
		Identify(store, rig, id);
		store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("rig"), 1});
		const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
		REQUIRE(store.SetParent(bone, rig));
		store.Set(bone, engine::scene::Bone{});
	}
	const auto sorted = engine::script::GetRigExport(store, "export/sorted");
	REQUIRE(std::string_view(sorted.Status) == "ok");
	CHECK(Field(Field(sorted.Value, "entities")->Items[0], "entity_id")->Text == "rig/a");
	const auto missing = engine::script::GetRigExport(store, "export/select", {"rig/missing"});
	CHECK(std::string_view(missing.Status) == "unknown_selection");

	engine::ecs::Store sparse("script.rigexport.sparse");
	const auto rig = sparse.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(sparse, rig, "rig/sparse");
	sparse.Set<engine::scene::Skeleton>(rig, {engine::core::Name("sparse"), 2});
	const auto bone = sparse.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(sparse.SetParent(bone, rig));
	engine::scene::Bone row;
	row.Joint = 1;
	sparse.Set(bone, row);
	CHECK(
		std::string_view(engine::script::GetRigExport(sparse, "export/sparse").Status) == "invalid_skeleton"
	);
}
