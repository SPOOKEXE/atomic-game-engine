#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/RigExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <string>
#include <string_view>
#include <vector>

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
	childBone.WorldFrame.Position = {10, 20, 30};
	store.Set(root, rootBone);
	store.Set(child, childBone);
	const auto nose = store.CreateInstance(engine::scene::RigKeypointClass(), "Nose");
	const auto leftEye = store.CreateInstance(engine::scene::RigKeypointClass(), "LeftEye");
	REQUIRE(store.SetParent(nose, rig));
	REQUIRE(store.SetParent(leftEye, rig));
	engine::scene::RigKeypoint nosePoint;
	nosePoint.Keypoint = engine::core::Name("nose");
	nosePoint.Joint = 1;
	nosePoint.Frame.Position = {0, 1, 0};
	store.Set(nose, nosePoint);
	engine::scene::RigKeypoint eyePoint;
	eyePoint.Keypoint = engine::core::Name("left_eye");
	eyePoint.Joint = 0;
	store.Set(leftEye, eyePoint);

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
	const auto &keypoints = Field(entities[0], "keypoints")->Items;
	REQUIRE(keypoints.size() == 2);
	CHECK(Field(keypoints[0], "keypoint_id")->Text == "rig/hero:keypoint:left_eye");
	std::vector<std::string> fields;
	for (const auto &[name, ignored] : keypoints[1].Entries) {
		(void)ignored;
		fields.push_back(name);
	}
	std::sort(fields.begin(), fields.end());
	CHECK(fields == std::vector<std::string>{"keypoint_id", "missing_reason", "name", "position", "state"});
	CHECK(Field(keypoints[1], "state")->Text == "present");
	CHECK(Field(keypoints[1], "missing_reason")->Tag == engine::script::ValueTag::Nil);
	const auto *position = Field(keypoints[1], "position");
	REQUIRE(position != nullptr);
	REQUIRE(position->Items.size() == 3);
	CHECK(position->Items[0].Number == 10);
	CHECK(position->Items[1].Number == 21);
	CHECK(position->Items[2].Number == 30);
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
	const auto point = store.CreateInstance(engine::scene::RigKeypointClass(), "Point");
	REQUIRE(store.SetParent(point, rig));
	engine::scene::RigKeypoint keypoint;
	keypoint.Keypoint = engine::core::Name("ok");
	keypoint.Joint = 0;
	store.Set(point, keypoint);
	const auto keypointBound = engine::script::GetRigExport(store, "export/keypoint-bound");
	REQUIRE(std::string_view(keypointBound.Status) == "ok");
	CHECK(
		Field(Field(keypointBound.Value, "entities")->Items[0], "keypoints")
			->Items[0]
			.Entries[0]
			.second.Text.size() <= engine::script::MAX_RIG_EXPORT_ID_BYTES
	);
	keypoint.Keypoint = engine::core::Name("too");
	store.Set(point, keypoint);
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/keypoint-too-long").Status) ==
		"invalid_keypoint_id"
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

TEST_CASE("rig export refuses malformed and duplicate authored keypoints", "[script][rigexport]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.keypoints");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/keypoints");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("keypoints"), 1});
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});
	for (const char *instanceName : {"First", "Second"}) {
		const auto point = store.CreateInstance(engine::scene::RigKeypointClass(), instanceName);
		REQUIRE(store.SetParent(point, rig));
		engine::scene::RigKeypoint row;
		row.Keypoint = engine::core::Name("shared");
		row.Joint = 0;
		store.Set(point, row);
	}
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/duplicate-keypoint").Status) ==
		"invalid_keypoint"
	);
	const auto malformed = store.CreateInstance(engine::scene::RigKeypointClass(), "Malformed");
	REQUIRE(store.SetParent(malformed, rig));
	engine::scene::RigKeypoint bad;
	bad.Keypoint = engine::core::Name("bad");
	bad.Joint = 1;
	store.Set(malformed, bad);
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/malformed-keypoint").Status) ==
		"invalid_keypoint"
	);
}
