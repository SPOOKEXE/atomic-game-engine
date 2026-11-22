// Partial vertex stream writes, weld points and normal recalculation on an
// `EditableMesh`, and the change record the uploader patches from.

#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.scene.editablemeshstreams")
TEST_DEPENDS("engine.scene.editablemesh")

using Catch::Approx;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
namespace scene = engine::scene;
using Stream = scene::EditableVertexStream;

namespace {
	template <class T, size_t N> std::span<const std::byte> Bytes(const std::array<T, N> &values) {
		return std::as_bytes(std::span(values));
	}

	// A skinned strip of `vertices` with a free channel, so a stream write has
	// something to leave alone.
	Entity MakeStrip(Store &store, uint32_t vertices) {
		const Entity mesh = store.CreateInstance(scene::EditableMeshClass(), "Strip");
		scene::EditableMeshGeometry geometry;
		for (uint32_t index = 0; index < vertices; index++) {
			geometry.Positions.push_back({float(index), float(index % 2), 0});
			geometry.Normals.push_back({0, 0, 1});
			geometry.UVs.push_back({float(index), 0});
			geometry.Colours.push_back({1, 1, 1});
			geometry.Alphas.push_back(0);
			geometry.Joints.push_back({uint16_t(index % 3), 0, 0, 0});
			geometry.Weights.push_back({1, 0, 0, 0});
			geometry.Attributes[2].push_back({float(index), 1, 2, 3});
		}
		for (uint32_t index = 0; index + 2 < vertices; index++)
			geometry.Indices.insert(geometry.Indices.end(), {index, index + 1, index + 2});
		REQUIRE(scene::ReplaceEditableMesh(store, mesh, geometry) == scene::EditableMeshCommit::Applied);
		return mesh;
	}

	// A UV sphere whose seam column is duplicated, as an exporter splits it:
	// column `segments` sits where column 0 does with its own UVs.
	Entity MakeSplitSphere(Store &store, uint32_t rings, uint32_t segments) {
		const Entity mesh = store.CreateInstance(scene::EditableMeshClass(), "Sphere");
		scene::EditableMeshGeometry geometry;
		const float pi = 3.14159265f;
		for (uint32_t ring = 0; ring <= rings; ring++) {
			const float polar = pi * float(ring) / float(rings);
			for (uint32_t segment = 0; segment <= segments; segment++) {
				const float azimuth = 2.0f * pi * float(segment % segments) / float(segments);
				geometry.Positions.push_back(
					{std::sin(polar) * std::cos(azimuth),
					 std::cos(polar),
					 std::sin(polar) * std::sin(azimuth)}
				);
				geometry.Normals.push_back({0, 1, 0});
				geometry.UVs.push_back({float(segment) / float(segments), float(ring) / float(rings)});
				geometry.Colours.push_back({1, 1, 1});
				geometry.Alphas.push_back(0);
			}
		}
		const uint32_t row = segments + 1;
		for (uint32_t ring = 0; ring < rings; ring++)
			for (uint32_t segment = 0; segment < segments; segment++) {
				const uint32_t a = ring * row + segment;
				geometry.Indices.insert(
					geometry.Indices.end(), {a, a + 1, a + row, a + 1, a + row + 1, a + row}
				);
			}
		REQUIRE(scene::ReplaceEditableMesh(store, mesh, geometry) == scene::EditableMeshCommit::Applied);
		return mesh;
	}
}

TEST_CASE(
	"a position stream write touches only positions and records one patchable revision",
	"[scene][editablemesh]"
) {
	Store store("streams.position");
	const Entity mesh = MakeStrip(store, 6);
	const scene::EditableMesh before = *store.Get<scene::EditableMesh>(mesh);

	const std::array<float, 6> moved{10, 11, 12, 20, 21, 22};
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Position, Bytes(moved), 2, 2));
	const auto *held = store.Get<scene::EditableMesh>(mesh);
	CHECK(held->Revision == before.Revision + 1);
	CHECK(held->Positions[2] == Vector3{10, 11, 12});
	CHECK(held->Positions[3] == Vector3{20, 21, 22});
	CHECK(held->Positions[1] == before.Positions[1]);
	CHECK(held->Positions[4] == before.Positions[4]);
	CHECK(held->Normals == before.Normals);
	CHECK(held->UVs == before.UVs);
	CHECK(held->Joints == before.Joints);
	CHECK(held->Weights == before.Weights);
	CHECK(held->Attributes == before.Attributes);
	CHECK(held->Indices == before.Indices);

	CHECK(held->StreamChanges.Covers(before.Revision, held->Revision));
	CHECK(held->StreamChanges.Streams == scene::EditableStreamBit(Stream::Position));
	CHECK(held->StreamChanges.First == 2);
	CHECK(held->StreamChanges.End == 4);

	// The same bytes again are not a change.
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Position, Bytes(moved), 2, 2));
	CHECK(store.Get<scene::EditableMesh>(mesh)->Revision == before.Revision + 1);

	// A second stream extends the same run, so a consumer two revisions
	// behind patches both.
	const std::array<float, 4> channel{9, 9, 9, 9};
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Attribute2, Bytes(channel), 5));
	held = store.Get<scene::EditableMesh>(mesh);
	CHECK(held->StreamChanges.Covers(before.Revision, held->Revision));
	CHECK(
		held->StreamChanges.Streams ==
		(scene::EditableStreamBit(Stream::Position) | scene::EditableStreamBit(Stream::Attribute2))
	);
	CHECK(held->StreamChanges.First == 2);
	CHECK(held->StreamChanges.End == 6);
}

TEST_CASE("a stream write that does not fit changes nothing", "[scene][editablemesh]") {
	Store store("streams.refused");
	const Entity mesh = MakeStrip(store, 4);
	const uint32_t revision = store.Get<scene::EditableMesh>(mesh)->Revision;

	const std::array<float, 6> two{1, 2, 3, 4, 5, 6};
	// Two records for a count of one, and a range past the end.
	CHECK_FALSE(scene::SetEditableVertexStream(store, mesh, Stream::Position, Bytes(two), 0, 1));
	CHECK_FALSE(scene::SetEditableVertexStream(store, mesh, Stream::Position, Bytes(two), 3));
	CHECK_FALSE(scene::SetEditableVertexStream(store, mesh, Stream::Position, Bytes(two), 5, 0));
	// Two positions are three UVs' worth of bytes, which is not whole records.
	CHECK_FALSE(scene::SetEditableVertexStream(store, mesh, Stream::UV, Bytes(two), 0, 2));
	const std::array<float, 3> infinite{0, std::numeric_limits<float>::infinity(), 0};
	CHECK_FALSE(scene::SetEditableVertexStream(store, mesh, Stream::Normal, Bytes(infinite), 0, 1));
	const std::array<uint32_t, 1> nowhere{4};
	CHECK_FALSE(scene::SetEditableVertexStream(store, mesh, Stream::Weld, Bytes(nowhere), 0, 1));
	CHECK(store.Get<scene::EditableMesh>(mesh)->Revision == revision);
}

TEST_CASE("colour, weld and new channel writes are not vertex patches", "[scene][editablemesh]") {
	Store store("streams.unpatched");
	const Entity mesh = MakeStrip(store, 3);

	const std::array<float, 4> colour{0.5f, 0.25f, 1, 0.75f};
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Colour, Bytes(colour), 1, 1));
	const auto *held = store.Get<scene::EditableMesh>(mesh);
	CHECK(held->Colours[1] == engine::core::Color3{0.5f, 0.25f, 1});
	CHECK(held->Alphas[1] == 0.75f);
	CHECK_FALSE(held->StreamChanges.Covers(held->Revision - 1, held->Revision));

	const std::array<float, 4> channel{1, 2, 3, 4};
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Attribute0, Bytes(channel), 2, 1));
	held = store.Get<scene::EditableMesh>(mesh);
	REQUIRE(held->Attributes[0].size() == 3);
	CHECK(held->Attributes[0][0] == scene::EditableAttribute{});
	CHECK(held->Attributes[0][2] == scene::EditableAttribute{1, 2, 3, 4});
	CHECK_FALSE(held->StreamChanges.Covers(held->Revision - 1, held->Revision));

	// The first weld fills every other vertex with itself.
	const std::array<uint32_t, 1> weld{0};
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Weld, Bytes(weld), 2, 1));
	held = store.Get<scene::EditableMesh>(mesh);
	CHECK(held->Welds == std::vector<uint32_t>{0, 1, 0});
	CHECK_FALSE(held->StreamChanges.Covers(held->Revision - 1, held->Revision));

	// A structural edit between two stream writes breaks the run.
	const uint32_t seen = held->Revision;
	REQUIRE(scene::AddTriangle(store, mesh, 0, 2, 1));
	const std::array<float, 3> position{5, 5, 5};
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Position, Bytes(position), 0, 1));
	held = store.Get<scene::EditableMesh>(mesh);
	CHECK_FALSE(held->StreamChanges.Covers(seen, held->Revision));
	CHECK(held->StreamChanges.Covers(held->Revision - 1, held->Revision));

	// Replacing the geometry drops the welds, which named the old vertices.
	scene::EditableMeshGeometry geometry;
	geometry.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	geometry.Normals.assign(3, {0, 0, 1});
	geometry.UVs.assign(3, {});
	geometry.Colours.assign(3, {1, 1, 1});
	geometry.Alphas.assign(3, 0);
	geometry.Indices = {0, 1, 2};
	REQUIRE(scene::ReplaceEditableMesh(store, mesh, geometry) == scene::EditableMeshCommit::Applied);
	CHECK(store.Get<scene::EditableMesh>(mesh)->Welds.empty());
}

TEST_CASE("recalculated normals are area weighted and shared across a welded seam", "[scene][editablemesh]") {
	Store store("streams.normals");
	constexpr uint32_t rings = 8;
	constexpr uint32_t segments = 12;
	const Entity mesh = MakeSplitSphere(store, rings, segments);
	const uint32_t row = segments + 1;
	const auto seamGap = [&] {
		const auto *held = store.Get<scene::EditableMesh>(mesh);
		float worst = 0.0f;
		for (uint32_t ring = 1; ring < rings; ring++) {
			const Vector3 left = held->Normals[ring * row];
			const Vector3 right = held->Normals[ring * row + segments];
			worst = std::max(worst, (left - right).Magnitude());
		}
		return worst;
	};

	// Unwelded, each side of the seam only sees its own half of the faces.
	REQUIRE(scene::RecalculateEditableMeshNormals(store, mesh));
	CHECK(seamGap() > 0.1f);

	std::vector<uint32_t> welds((rings + 1) * row);
	for (uint32_t vertex = 0; vertex < welds.size(); vertex++)
		welds[vertex] = vertex % row == segments ? vertex - segments : vertex;
	REQUIRE(scene::SetEditableVertexStream(store, mesh, Stream::Weld, std::as_bytes(std::span(welds))));
	const uint32_t revision = store.Get<scene::EditableMesh>(mesh)->Revision;
	REQUIRE(scene::RecalculateEditableMeshNormals(store, mesh));
	CHECK(seamGap() == 0.0f);
	const auto *held = store.Get<scene::EditableMesh>(mesh);
	CHECK(held->Revision == revision + 1);
	CHECK(held->StreamChanges.Streams == scene::EditableStreamBit(Stream::Normal));

	// On a sphere every normal points out along its position.
	for (uint32_t ring = 1; ring < rings; ring++)
		for (uint32_t segment = 0; segment <= segments; segment++) {
			const uint32_t vertex = ring * row + segment;
			const Vector3 normal = held->Normals[vertex];
			const Vector3 out = held->Positions[vertex];
			CHECK(normal.Magnitude() == Approx(1.0f));
			CHECK(normal.Dot(out) / out.Magnitude() > 0.99f);
		}

	// A range writes only its own vertices.
	REQUIRE(scene::SetVertexNormal(store, mesh, 0, Vector3{1, 0, 0}));
	REQUIRE(scene::SetVertexNormal(store, mesh, 1, Vector3{1, 0, 0}));
	REQUIRE(scene::RecalculateEditableMeshNormals(store, mesh, 1, 1));
	held = store.Get<scene::EditableMesh>(mesh);
	CHECK(held->Normals[0] == Vector3{1, 0, 0});
	CHECK(held->Normals[1].Y > 0.9f);
	CHECK_FALSE(scene::RecalculateEditableMeshNormals(store, mesh, uint32_t(welds.size()), 1));
}
