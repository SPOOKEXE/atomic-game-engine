#include <engine/assets/Animation.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/MeshCatalogue.hpp>
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
	engine::scene::Visual visual;
	visual.Mesh = engine::core::Name("characters/hero.amesh");
	store.Set(rig, visual);
	engine::scene::MeshSkinning skinning;
	skinning.JointCount = 2;
	skinning.VertexCount = 2;
	skinning.Vertices = {
		{{0, 1, 65535, 65535}, {32768, 32767, 0, 0}},
		{{1, 0, 0, 0}, {65535, 0, 0, 0}},
	};
	REQUIRE(engine::scene::RecordMesh(store, visual.Mesh, 1, {}, skinning));
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
	const auto *exportedSkinning = Field(entities[0], "skinning");
	REQUIRE(exportedSkinning != nullptr);
	CHECK(Field(*exportedSkinning, "available")->Boolean);
	CHECK(Field(*exportedSkinning, "unavailable_reason")->Tag == engine::script::ValueTag::Nil);
	CHECK(Field(*exportedSkinning, "mesh_id")->Text == "characters/hero.amesh");
	CHECK(Field(*exportedSkinning, "position_space")->Text == "mesh_object");
	CHECK(Field(*exportedSkinning, "joint_index_space")->Text == "skeleton_palette_slot");
	CHECK(Field(*exportedSkinning, "weight_encoding")->Text == "uint16_unorm");
	CHECK(Field(*exportedSkinning, "weight_denominator")->Number == 65535);
	const auto &skinVertices = Field(*exportedSkinning, "vertices")->Items;
	REQUIRE(skinVertices.size() == 2);
	CHECK(Field(skinVertices[0], "vertex_index")->Number == 0);
	const auto &influences = Field(skinVertices[0], "influences")->Items;
	REQUIRE(influences.size() == 2);
	CHECK(Field(influences[0], "joint_id")->Text == "rig/hero:joint:0");
	CHECK(Field(influences[1], "joint_slot")->Number == 1);
	CHECK(Field(influences[0], "weight")->Number == 32768);
	CHECK(Field(influences[1], "weight")->Number == 32767);
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

TEST_CASE(
	"rig export decodes buffered clips into exact translation and rotation keys", "[script][rigexport]"
) {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.clip");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/clip");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("clip-rig"), 1});
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Root");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});
	const auto buffer =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "WaveBuffer");
	engine::assets::AnimationData data;
	data.Duration = store.Time().Delta;
	engine::assets::AnimationChannel channel;
	channel.Joint = 0;
	channel.Keys = {{0.0f, {}}, {store.Time().Delta, {}}};
	channel.Keys[1].Transform.Position.X = 3.0f;
	data.Channels.push_back(std::move(channel));
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::Animation::Write(writer, data));
	store.Set(
		buffer, engine::scene::AnimationBuffer{std::vector(writer.Bytes().begin(), writer.Bytes().end()), 1}
	);
	const auto clip =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Wave");
	Identify(store, clip, "clip/wave");
	store.Set(
		clip,
		engine::scene::AnimationClip{
			engine::core::Name("asset/ignored"), engine::core::Name("clip-rig"), buffer
		}
	);

	const auto exported = engine::script::GetRigExport(store, "export/clip");
	REQUIRE(std::string_view(exported.Status) == "ok");
	const auto &clips = Field(Field(exported.Value, "entities")->Items[0], "clips")->Items;
	REQUIRE(clips.size() == 1);
	CHECK(Field(clips[0], "clip_id")->Text == "clip/wave");
	CHECK(Field(clips[0], "name")->Text == "Wave");
	CHECK(Field(clips[0], "end_tick")->Number == 1);
	const auto &channels = Field(clips[0], "channels")->Items;
	REQUIRE(channels.size() == 2);
	CHECK(Field(channels[0], "property")->Text == "rotation");
	CHECK(Field(channels[1], "property")->Text == "translation");
	CHECK(Field(Field(channels[1], "keys")->Items[1], "time")->Entries[0].second.Number == 1);
}

TEST_CASE(
	"rig export declares missing and over-limit mesh skinning without inventing rows", "[script][rigexport]"
) {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.skinning-limits");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/skinning-limits");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("skinning-limits"), 1});
	engine::scene::Visual visual;
	visual.Mesh = engine::core::Name("characters/limits.amesh");
	store.Set(rig, visual);
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});

	auto exported = engine::script::GetRigExport(store, "export/missing-skinning");
	REQUIRE(std::string_view(exported.Status) == "ok");
	const auto *missing = Field(Field(exported.Value, "entities")->Items[0], "skinning");
	REQUIRE(missing != nullptr);
	CHECK_FALSE(Field(*missing, "available")->Boolean);
	CHECK(Field(*missing, "unavailable_reason")->Text == "mesh skinning source is unavailable in this world");
	CHECK(Field(*missing, "vertices")->Items.empty());

	engine::scene::MeshSkinning skinning;
	skinning.JointCount = 1;
	skinning.VertexCount = engine::script::MAX_RIG_EXPORT_SKIN_VERTICES + 1;
	skinning.Vertices.resize(engine::scene::MAXIMUM_RETAINED_SKINNING_VERTICES);
	REQUIRE(engine::scene::RecordMesh(store, visual.Mesh, 1, {}, skinning));
	exported = engine::script::GetRigExport(store, "export/over-limit-skinning");
	REQUIRE(std::string_view(exported.Status) == "ok");
	const auto *overLimit = Field(Field(exported.Value, "entities")->Items[0], "skinning");
	REQUIRE(overLimit != nullptr);
	CHECK_FALSE(Field(*overLimit, "available")->Boolean);
	CHECK(
		Field(*overLimit, "unavailable_reason")->Text ==
		"mesh skinning vertices exceed the 1024 response limit"
	);
	CHECK(Field(*overLimit, "vertices")->Items.empty());
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

TEST_CASE("rig export refuses clip ticks beyond exact ScriptValue integers", "[script][rigexport]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.tick-overflow");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/tick-overflow");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("tick-overflow"), 1});
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});
	store.AdvanceTick(std::bit_cast<float>(uint32_t{0x2B800000})); // 2^-40 seconds.
	const auto buffer =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "Buffer");
	engine::assets::AnimationData data;
	data.Duration = std::bit_cast<float>(uint32_t{0x5F000000}); // 2^63 seconds.
	engine::assets::AnimationChannel channel;
	channel.Joint = 0;
	channel.Keys = {{0.0f, {}}, {data.Duration, {}}};
	data.Channels.push_back(std::move(channel));
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::Animation::Write(writer, data));
	store.Set(
		buffer, engine::scene::AnimationBuffer{std::vector(writer.Bytes().begin(), writer.Bytes().end()), 1}
	);
	const auto clip =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Clip");
	Identify(store, clip, "clip/tick-overflow");
	store.Set(clip, engine::scene::AnimationClip{{}, engine::core::Name("tick-overflow"), buffer});
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/tick-overflow").Status) ==
		"unaligned_clip_time"
	);
}

TEST_CASE("rig export refuses asset-only, dangling, and malformed buffered clips", "[script][rigexport]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.invalid-clip");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/invalid-clip");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("invalid-clip"), 1});
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});
	const auto clip =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Clip");
	Identify(store, clip, "clip/invalid");
	engine::scene::AnimationClip definition;
	definition.Asset = engine::core::Name("asset/only");
	definition.Rig = engine::core::Name("invalid-clip");
	store.Set(clip, definition);
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/asset-only").Status) == "asset_only_clip"
	);
	definition.Buffer = engine::ecs::Entity{999};
	store.Set(clip, definition);
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/dangling").Status) ==
		"dangling_clip_buffer"
	);
	const auto buffer =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "Buffer");
	store.Set(buffer, engine::scene::AnimationBuffer{{std::byte{0}}, 1});
	definition.Buffer = buffer;
	store.Set(clip, definition);
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/malformed").Status) ==
		"malformed_clip_buffer"
	);
}

TEST_CASE(
	"rig export excludes other rigs and rejects decoded clip contract violations", "[script][rigexport]"
) {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.rigexport.clip-contract");
	const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(store, rig, "rig/clip-contract");
	store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("clip-contract"), 1});
	const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});
	const auto buffer =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "Buffer");
	const auto clip =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Clip");
	Identify(store, clip, "clip/contract");
	engine::assets::AnimationData data;
	data.Duration = store.Time().Delta;
	engine::assets::AnimationChannel channel;
	channel.Joint = 0;
	channel.Keys = {{0.0f, {}}, {data.Duration, {}}};
	data.Channels.push_back(channel);
	auto bake = [&] {
		engine::core::ByteWriter writer;
		REQUIRE(engine::assets::Animation::Write(writer, data));
		store.Set(
			buffer,
			engine::scene::AnimationBuffer{std::vector(writer.Bytes().begin(), writer.Bytes().end()), 1}
		);
	};
	bake();
	store.Set(clip, engine::scene::AnimationClip{{}, engine::core::Name("clip-contract"), buffer});
	const auto foreign =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Foreign");
	Identify(store, foreign, "clip/foreign");
	store.Set(
		foreign, engine::scene::AnimationClip{{}, engine::core::Name("other-rig"), engine::ecs::NULL_ENTITY}
	);
	const auto scoped = engine::script::GetRigExport(store, "export/scoped");
	REQUIRE(std::string_view(scoped.Status) == "ok");
	const auto &clips = Field(Field(scoped.Value, "entities")->Items[0], "clips")->Items;
	REQUIRE(clips.size() == 1);
	const auto &key = Field(Field(clips[0], "channels")->Items[1], "keys")->Items[1];
	CHECK(Field(key, "value")->Items.size() == 3);
	const auto *time = Field(key, "time");
	REQUIRE(time != nullptr);
	CHECK(Field(*time, "tick")->Number == 1);
	CHECK(Field(*time, "seconds_numerator")->Number == 8'947'849);
	CHECK(Field(*time, "seconds_denominator")->Number == 536'870'912);

	const auto duplicate =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Duplicate");
	Identify(store, duplicate, "clip/contract");
	store.Set(duplicate, engine::scene::AnimationClip{{}, engine::core::Name("clip-contract"), buffer});
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/duplicate").Status) == "invalid_clip_id"
	);
	store.Set(duplicate, engine::scene::AnimationClip{{}, engine::core::Name("other-rig"), buffer});

	std::vector<std::byte> trailing = store.Get<engine::scene::AnimationBuffer>(buffer)->Data;
	trailing.push_back(std::byte{0});
	store.Set(buffer, engine::scene::AnimationBuffer{std::move(trailing), 2});
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/trailing").Status) ==
		"malformed_clip_buffer"
	);

	data.Duration = 0.5f;
	data.Channels[0].Keys = {{0.0f, {}}, {data.Duration, {}}};
	bake();
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/off-grid").Status) ==
		"unaligned_clip_time"
	);
	data.Duration = store.Time().Delta;
	data.Channels[0].Joint = 1;
	data.Channels[0].Keys = {{0.0f, {}}, {data.Duration, {}}};
	bake();
	CHECK(
		std::string_view(engine::script::GetRigExport(store, "export/missing-slot").Status) ==
		"invalid_clip_channel"
	);
}

TEST_CASE("rig export bounds clips and aggregate animation keys", "[script][rigexport]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store clipsStore("script.rigexport.clip-limit");
	const auto rig = clipsStore.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(clipsStore, rig, "rig/clip-limit");
	clipsStore.Set<engine::scene::Skeleton>(rig, {engine::core::Name("clip-limit"), 1});
	const auto bone = clipsStore.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(clipsStore.SetParent(bone, rig));
	clipsStore.Set(bone, engine::scene::Bone{});
	const auto buffer = clipsStore.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "Buffer"
	);
	engine::assets::AnimationData small;
	small.Duration = clipsStore.Time().Delta;
	small.Channels = {{{0, {{0.0f, {}}, {small.Duration, {}}}}}};
	engine::core::ByteWriter smallWriter;
	REQUIRE(engine::assets::Animation::Write(smallWriter, small));
	clipsStore.Set(
		buffer,
		engine::scene::AnimationBuffer{std::vector(smallWriter.Bytes().begin(), smallWriter.Bytes().end()), 1}
	);
	for (size_t index = 0; index <= engine::script::MAX_RIG_EXPORT_CLIPS_PER_ENTITY; ++index) {
		const auto clip =
			clipsStore.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Clip");
		Identify(clipsStore, clip, "clip/" + std::to_string(index));
		clipsStore.Set(clip, engine::scene::AnimationClip{{}, engine::core::Name("clip-limit"), buffer});
	}
	CHECK(
		std::string_view(engine::script::GetRigExport(clipsStore, "export/clip-limit").Status) ==
		"resource_limit"
	);

	engine::ecs::Store keysStore("script.rigexport.key-limit");
	const auto keysRig = keysStore.CreateInstance(engine::scene::PartClass(), "Rig");
	Identify(keysStore, keysRig, "rig/key-limit");
	keysStore.Set<engine::scene::Skeleton>(keysRig, {engine::core::Name("key-limit"), 1});
	const auto keysBone = keysStore.CreateInstance(engine::scene::BoneClass(), "Bone");
	REQUIRE(keysStore.SetParent(keysBone, keysRig));
	keysStore.Set(keysBone, engine::scene::Bone{});
	keysStore.AdvanceTick(1.0f / 4096.0f);
	engine::assets::AnimationData large;
	large.Duration = 1.0f;
	large.Channels.emplace_back();
	large.Channels[0].Joint = 0;
	for (size_t index = 0; index < engine::script::MAX_RIG_EXPORT_KEYS_PER_CHANNEL; ++index)
		large.Channels[0].Keys.push_back({static_cast<float>(index) / 4096.0f, {}});
	engine::core::ByteWriter largeWriter;
	REQUIRE(engine::assets::Animation::Write(largeWriter, large));
	const auto keysBuffer =
		keysStore.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "Buffer");
	keysStore.Set(
		keysBuffer,
		engine::scene::AnimationBuffer{std::vector(largeWriter.Bytes().begin(), largeWriter.Bytes().end()), 1}
	);
	for (size_t index = 0; index < 5; ++index) {
		const auto clip =
			keysStore.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Clip");
		Identify(keysStore, clip, "keys/" + std::to_string(index));
		keysStore.Set(clip, engine::scene::AnimationClip{{}, engine::core::Name("key-limit"), keysBuffer});
	}
	CHECK(
		std::string_view(engine::script::GetRigExport(keysStore, "export/key-limit").Status) ==
		"resource_limit"
	);
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
