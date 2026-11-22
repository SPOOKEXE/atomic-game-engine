// Skin influences, free attribute channels and packed buffers on an
// `EditableMesh`, and the bone numbering a script rig gets from `SetBones`.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.scene.editablemeshskin")

using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
namespace scene = engine::scene;

namespace {
	Entity MakeMesh(Store &store, uint32_t vertices) {
		const Entity mesh = store.CreateInstance(scene::EditableMeshClass(), "Mesh");
		for (uint32_t index = 0; index < vertices; index++)
			scene::AddVertex(store, mesh, Vector3{float(index), 0, 0});
		return mesh;
	}

	template <class T> void Append(std::vector<std::byte> &bytes, const T &value) {
		const auto *first = reinterpret_cast<const std::byte *>(&value);
		bytes.insert(bytes.end(), first, first + sizeof(T));
	}
}

TEST_CASE("vertex skin round trips and keeps arrays parallel", "[scene][editablemesh]") {
	Store store("skin");
	const Entity mesh = MakeMesh(store, 2);
	const auto *held = store.Get<scene::EditableMesh>(mesh);
	CHECK(held->Joints.empty());

	const std::array<uint16_t, 2> joints{3, 7};
	const std::array<float, 2> weights{0.25f, 0.75f};
	const uint32_t before = held->Revision;
	REQUIRE(scene::SetVertexSkin(store, mesh, 1, joints, weights));
	held = store.Get<scene::EditableMesh>(mesh);
	REQUIRE(held->Joints.size() == 2);
	CHECK(held->Joints[1] == scene::EditableJoints{3, 7, 0, 0});
	CHECK(held->Weights[1] == scene::EditableWeights{0.25f, 0.75f, 0, 0});
	CHECK(held->Joints[0] == scene::EditableJoints{});
	CHECK(held->Revision == before + 1);

	// The same influences again change nothing.
	REQUIRE(scene::SetVertexSkin(store, mesh, 1, joints, weights));
	CHECK(store.Get<scene::EditableMesh>(mesh)->Revision == before + 1);

	scene::AddVertex(store, mesh, Vector3{});
	CHECK(store.Get<scene::EditableMesh>(mesh)->Joints.size() == 3);

	const std::array<uint16_t, 1> outside{scene::MAX_JOINTS};
	const std::array<float, 1> one{1.0f};
	const std::array<float, 1> negative{-1.0f};
	CHECK_FALSE(scene::SetVertexSkin(store, mesh, 0, outside, one));
	CHECK_FALSE(scene::SetVertexSkin(store, mesh, 0, std::span(joints.data(), 1), negative));
	CHECK_FALSE(scene::SetVertexSkin(store, mesh, 9, std::span(joints.data(), 1), one));

	REQUIRE(scene::SetVertexAttribute(store, mesh, 2, 3, {1, 2, 3, 4}));
	CHECK(store.Get<scene::EditableMesh>(mesh)->Attributes[3][2] == scene::EditableAttribute{1, 2, 3, 4});
	CHECK(store.Get<scene::EditableMesh>(mesh)->Attributes[3].size() == 3);
	CHECK_FALSE(scene::SetVertexAttribute(store, mesh, 0, 4, {}));

	REQUIRE(scene::ClearEditableMesh(store, mesh));
	CHECK(store.Get<scene::EditableMesh>(mesh)->Joints.empty());
	CHECK(store.Get<scene::EditableMesh>(mesh)->Attributes[3].empty());
}

TEST_CASE("bulk geometry validates skin and attribute arrays", "[scene][editablemesh]") {
	scene::EditableMeshGeometry geometry;
	geometry.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
	geometry.Normals.assign(3, Vector3{0, 1, 0});
	geometry.UVs.assign(3, {});
	geometry.Colours.assign(3, {1, 1, 1});
	geometry.Alphas.assign(3, 0);
	geometry.Indices = {0, 2, 1};
	CHECK(scene::PrepareEditableMesh(geometry).Valid);

	auto skinned = geometry;
	skinned.Joints.assign(3, {0, 1, 0, 0});
	skinned.Weights.assign(3, {0.5f, 0.5f, 0, 0});
	CHECK(scene::PrepareEditableMesh(skinned).Valid);
	CHECK(scene::PrepareEditableMesh(skinned).Signature != scene::PrepareEditableMesh(geometry).Signature);

	auto unweighted = skinned;
	unweighted.Weights.clear();
	CHECK_FALSE(scene::PrepareEditableMesh(unweighted).Valid);
	auto shortSkin = skinned;
	shortSkin.Joints.pop_back();
	shortSkin.Weights.pop_back();
	CHECK_FALSE(scene::PrepareEditableMesh(shortSkin).Valid);
	auto farJoint = skinned;
	farJoint.Joints[0][0] = scene::MAX_JOINTS;
	CHECK_FALSE(scene::PrepareEditableMesh(farJoint).Valid);
	auto channel = geometry;
	channel.Attributes[2].assign(2, {});
	CHECK_FALSE(scene::PrepareEditableMesh(channel).Valid);
	channel.Attributes[2].assign(3, {0, 0, 0, std::numeric_limits<float>::quiet_NaN()});
	CHECK_FALSE(scene::PrepareEditableMesh(channel).Valid);
}

TEST_CASE("packed buffers decode interleaved and block layouts", "[scene][editablemesh]") {
	// Interleaved position and skin, 40 bytes a vertex.
	std::vector<std::byte> vertices;
	for (uint16_t vertex = 0; vertex < 3; vertex++) {
		Append(vertices, float(vertex));
		Append(vertices, 2.0f);
		Append(vertices, 3.0f);
		for (uint16_t slot = 0; slot < 4; slot++)
			Append(vertices, uint16_t(vertex + slot));
		Append(vertices, 1.0f);
		Append(vertices, 0.0f);
		Append(vertices, 0.0f);
		Append(vertices, 0.0f);
		Append(vertices, uint32_t(0));
	}
	// A block of one attribute channel after the vertices.
	const uint32_t attributeOffset = static_cast<uint32_t>(vertices.size());
	for (int vertex = 0; vertex < 3; vertex++)
		for (int component = 0; component < 4; component++)
			Append(vertices, float(vertex * 10 + component));
	std::vector<std::byte> indices;
	for (const uint16_t index : {0, 2, 1})
		Append(indices, index);

	scene::EditableBufferLayout layout;
	layout.VertexCount = 3;
	layout.WideIndices = false;
	layout.Position = {true, 0, 40};
	layout.Joints = {true, 12, 40};
	layout.Weights = {true, 20, 40};
	layout.Attributes[1] = {true, attributeOffset, 0};
	scene::EditableMeshGeometry geometry;
	REQUIRE(scene::DecodeEditableMeshBuffers(layout, vertices, indices, geometry).empty());
	CHECK(geometry.Positions[2] == Vector3{2, 2, 3});
	CHECK(geometry.Normals[0] == Vector3{0, 1, 0});
	CHECK(geometry.Joints[1] == scene::EditableJoints{1, 2, 3, 4});
	CHECK(geometry.Weights[2] == scene::EditableWeights{1, 0, 0, 0});
	CHECK(geometry.Attributes[1][2] == scene::EditableAttribute{20, 21, 22, 23});
	CHECK(geometry.Indices == std::vector<uint32_t>{0, 2, 1});
	CHECK(scene::PrepareEditableMesh(geometry).Valid);

	SECTION("a field past the end is refused") {
		layout.Attributes[1].Offset = attributeOffset + 4;
		CHECK_FALSE(scene::DecodeEditableMeshBuffers(layout, vertices, indices, geometry).empty());
	}
	SECTION("a stride narrower than the field is refused") {
		layout.Position.Stride = 8;
		CHECK_FALSE(scene::DecodeEditableMeshBuffers(layout, vertices, indices, geometry).empty());
	}
	SECTION("joints need weights") {
		layout.Weights.Present = false;
		CHECK_FALSE(scene::DecodeEditableMeshBuffers(layout, vertices, indices, geometry).empty());
	}
	SECTION("a partial index is refused") {
		indices.pop_back();
		CHECK_FALSE(scene::DecodeEditableMeshBuffers(layout, vertices, indices, geometry).empty());
	}
	SECTION("a count past the ceiling is refused before reading") {
		layout.VertexCount = scene::MAXIMUM_EDITABLE_MESH_VERTICES + 1;
		CHECK_FALSE(scene::DecodeEditableMeshBuffers(layout, vertices, indices, geometry).empty());
	}
	SECTION("a position is required") {
		layout.Position.Present = false;
		CHECK_FALSE(scene::DecodeEditableMeshBuffers(layout, vertices, indices, geometry).empty());
	}
}

TEST_CASE("skin and attribute channels survive a snapshot", "[scene][editablemesh]") {
	Store source("skin.snapshot");
	const Entity mesh = MakeMesh(source, 2);
	const std::array<uint16_t, 2> joints{1, 2};
	const std::array<float, 2> weights{0.5f, 0.5f};
	REQUIRE(scene::SetVertexSkin(source, mesh, 0, joints, weights));
	REQUIRE(scene::SetVertexAttribute(source, mesh, 1, 0, {4, 3, 2, 1}));

	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));
	Store restored("skin.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	const auto *held = restored.Get<scene::EditableMesh>(mesh);
	REQUIRE(held != nullptr);
	CHECK(held->Joints == source.Get<scene::EditableMesh>(mesh)->Joints);
	CHECK(held->Weights == source.Get<scene::EditableMesh>(mesh)->Weights);
	CHECK(held->Attributes == source.Get<scene::EditableMesh>(mesh)->Attributes);
}

TEST_CASE("SetBones numbers a script rig parent first", "[scene][skinning]") {
	Store store("bones");
	const Entity rig =
		store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("SkinnedMeshPart")), "Rig");
	const auto bone = [&](Entity parent, const char *name) {
		const Entity made = store.CreateInstance(scene::BoneClass(), name);
		REQUIRE(store.SetParent(made, parent));
		return made;
	};
	const Entity hips = bone(rig, "Hips");
	const Entity spine = bone(hips, "Spine");
	const Entity head = bone(spine, "Head");

	CHECK_FALSE(scene::BindSkeletonBones(store, rig, std::array{hips, spine}).empty());
	CHECK_FALSE(scene::BindSkeletonBones(store, rig, std::array{spine, hips, head}).empty());
	CHECK_FALSE(scene::BindSkeletonBones(store, rig, std::array{hips, hips, head}).empty());
	CHECK_FALSE(scene::BindSkeletonBones(store, hips, std::array{spine, head}).empty());

	REQUIRE(scene::BindSkeletonBones(store, rig, std::array{hips, spine, head}).empty());
	CHECK(store.Get<scene::Skeleton>(rig)->JointCount == 3);
	CHECK(store.Get<scene::Bone>(hips)->ParentJoint == scene::NO_JOINT);
	CHECK(store.Get<scene::Bone>(spine)->Joint == 1);
	CHECK(store.Get<scene::Bone>(spine)->ParentJoint == 0);
	CHECK(store.Get<scene::Bone>(head)->ParentJoint == 1);
}
