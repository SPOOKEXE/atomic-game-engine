// A mesh's collision geometry is the same on every host that has the bytes.
//
// **The claim is that a headless server and a client agree about a hull.** A
// `scene::Collider` names its geometry with a string, resolved through
// `scene::CollisionShapes`, and until v0.17 the only thing that ever filled that
// table was the client, inline, on the frame an asset arrived. A server had mesh
// colliders it could not resolve, so every one of them fell back silently to the
// part's bound - a client and a server disagreeing about where a player stands
// while both look self-consistent.
//
// What is tested here is that the two paths in - a client's arriving asset and a
// server's content store - produce the same geometry out, that the built-ins are
// present before any content is, and that one unreadable entry in a store costs
// only that entry.
//
// The hull and the soup themselves are tested in `collision`; this suite is
// about the wiring.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Builtin.hpp>
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/CollisionContent.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ChunkStore;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::assets::MeshData;
using engine::assets::MeshVertex;
using engine::assets::SignatureBytes;
using engine::core::Name;
using engine::game::AddBuiltinCollisionShapes;
using engine::game::AddCollisionShapes;
using engine::game::AddCollisionShapesFrom;
using engine::game::MergeCollisionShapes;
using engine::game::RecordBuiltinCollisionShapes;
using engine::scene::CollisionShapes;
using engine::scene::CollisionShapesOf;

namespace fs = std::filesystem;

namespace {
	MeshVertex At(float x, float y, float z) {
		return MeshVertex{{x, y, z}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}};
	}

	// A unit box, which is the smallest thing with a hull worth checking.
	MeshData Box() {
		MeshData mesh;
		mesh.Vertices = {
			At(-1.0f, -1.0f, -1.0f),
			At(1.0f, -1.0f, -1.0f),
			At(1.0f, 1.0f, -1.0f),
			At(-1.0f, 1.0f, -1.0f),
			At(-1.0f, -1.0f, 1.0f),
			At(1.0f, -1.0f, 1.0f),
			At(1.0f, 1.0f, 1.0f),
			At(-1.0f, 1.0f, 1.0f),
		};
		mesh.Indices = {
			0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
			3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
		};
		mesh.ComputeBounds();
		return mesh;
	}

	std::vector<std::byte> Encoded(const MeshData &mesh) {
		engine::core::ByteWriter writer;
		REQUIRE(engine::assets::Mesh::Write(writer, mesh));
		const std::span<const std::byte> written = writer.Bytes();
		return std::vector<std::byte>(written.begin(), written.end());
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	struct Tree {
		fs::path Root;

		Tree() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-collision-content-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		~Tree() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}
	};

	// Puts one asset in the store as a single chunk and names it in the manifest.
	void Publish(
		ChunkStore &store,
		Manifest &manifest,
		std::string name,
		AssetKind kind,
		const std::vector<std::byte> &bytes
	) {
		const engine::assets::ContentHash hash = Hasher::Of(bytes);
		REQUIRE(store.Write(hash, bytes));
		manifest.AddAsset(
			std::move(name), kind, {ChunkEntry{.Hash = hash, .Bytes = static_cast<uint32_t>(bytes.size())}}
		);
	}

	// Names it in the manifest and deliberately does not write the chunk.
	void Announce(Manifest &manifest, std::string name, AssetKind kind, const std::vector<std::byte> &bytes) {
		manifest.AddAsset(
			std::move(name),
			kind,
			{ChunkEntry{.Hash = Hasher::Of(bytes), .Bytes = static_cast<uint32_t>(bytes.size())}}
		);
	}
}

TEST_CASE("a mesh bakes into both a hull and a triangle soup", "[game][collision]") {
	// **Both, because which one a part wants is authoring.**
	// `BasePart.CollisionShape` can change after the mesh has arrived, so baking
	// only the kind in use when the bytes landed would leave the other
	// unreachable until a reload.
	CollisionShapes shapes;
	AddCollisionShapes(shapes, Name("props/crate.amesh"), Box());

	const auto *hull = shapes.FindHull(Name("props/crate.amesh"));
	const auto *soup = shapes.FindMesh(Name("props/crate.amesh"));
	REQUIRE(hull != nullptr);
	REQUIRE(soup != nullptr);

	CHECK(hull->Solid());
	CHECK(soup->TriangleCount() == 12);
}

TEST_CASE("a nameless mesh puts no row in the table", "[game][collision]") {
	// Nothing could ever ask for it, and the row would be one more thing every
	// lookup walks past.
	CollisionShapes shapes;
	AddCollisionShapes(shapes, Name(), Box());
	CHECK(shapes.HullCount() == 0);
	CHECK(shapes.MeshCount() == 0);
}

TEST_CASE("a mesh with no vertices registers nothing", "[game][collision]") {
	// **Rather than an empty shape.** `SupportPoint` answers the origin for a
	// hull with no points, so registering one gives a part a collider that is a
	// single point at its own position - it stops nothing and says nothing. An
	// unresolved name falls back to the part's bound, which is visible.
	CollisionShapes shapes;
	AddCollisionShapes(shapes, Name("props/empty.amesh"), MeshData{});
	CHECK(shapes.HullCount() == 0);
	CHECK(shapes.MeshCount() == 0);
}

TEST_CASE("the built-in meshes have collision geometry", "[game][collision]") {
	// **The gap that made a `MeshPart` set to `Cube` collide as its bound on
	// every host including the client.** `MakeBuiltin` generates the six rather
	// than shipping files, so they never travel the content path.
	CollisionShapes shapes;
	AddBuiltinCollisionShapes(shapes);

	CHECK(shapes.HullCount() == engine::assets::BUILTIN_MESH_COUNT);
	CHECK(shapes.MeshCount() == engine::assets::BUILTIN_MESH_COUNT);

	for (uint8_t index = 0; index < engine::assets::BUILTIN_MESH_COUNT; index++) {
		const auto which = static_cast<engine::assets::BuiltinMesh>(index);
		const Name name(engine::assets::BuiltinName(which));

		INFO(name.Text());
		const auto *hull = shapes.FindHull(name);
		REQUIRE(hull != nullptr);
		CHECK_FALSE(hull->Points.empty());
		CHECK(shapes.FindMesh(name) != nullptr);

		// **Every one but `Plane`, which is flat and has no volume to hull.**
		// `BuildConvexHull` says so: a degenerate input keeps its points and
		// gets no faces, so `Solid()` is false and support queries still answer.
		// A quad collides as a quad, which is what a quad should do.
		CHECK(hull->Solid() == (which != engine::assets::BuiltinMesh::Plane));
	}
}

TEST_CASE("baking the built-ins twice is the same table", "[game][collision]") {
	// A world may be prepared again - reopened, restored from a replay - and the
	// call is on that path.
	CollisionShapes once;
	AddBuiltinCollisionShapes(once);
	CollisionShapes twice = once;
	AddBuiltinCollisionShapes(twice);

	CHECK(twice.HullCount() == once.HullCount());
	CHECK(twice.MeshCount() == once.MeshCount());
}

TEST_CASE("a content store bakes its meshes and nothing else", "[game][collision]") {
	Tree tree;
	auto opened = ChunkStore::Open(tree.Root, true);
	REQUIRE(opened.has_value());

	Manifest manifest;
	Publish(*opened, manifest, "props/crate.amesh", AssetKind::Mesh, Encoded(Box()));
	Publish(
		*opened, manifest, "art/skin.atex", AssetKind::Texture, Bytes("not a mesh and not claimed to be")
	);

	CollisionShapes shapes;
	CHECK(AddCollisionShapesFrom(shapes, *opened, manifest) == 1);

	CHECK(shapes.FindHull(Name("props/crate.amesh")) != nullptr);
	CHECK(shapes.FindHull(Name("art/skin.atex")) == nullptr);
	CHECK(shapes.HullCount() == 1);
}

TEST_CASE("a store and an arriving asset bake the same hull", "[game][collision]") {
	// **The claim the whole file exists for.** The server reads its store, the
	// client is handed the bytes, and physics on either side has to reach the
	// same answer or the two disagree about where a body stops.
	Tree tree;
	auto opened = ChunkStore::Open(tree.Root, true);
	REQUIRE(opened.has_value());

	Manifest manifest;
	Publish(*opened, manifest, "props/crate.amesh", AssetKind::Mesh, Encoded(Box()));

	CollisionShapes served;
	REQUIRE(AddCollisionShapesFrom(served, *opened, manifest) == 1);

	// What the client does with the same bytes, which is what it does with the
	// mesh it decoded to draw.
	CollisionShapes delivered;
	AddCollisionShapes(delivered, Name("props/crate.amesh"), Box());

	const auto *left = served.FindHull(Name("props/crate.amesh"));
	const auto *right = delivered.FindHull(Name("props/crate.amesh"));
	REQUIRE(left != nullptr);
	REQUIRE(right != nullptr);

	REQUIRE(left->Points.size() == right->Points.size());
	for (size_t index = 0; index < left->Points.size(); index++) {
		INFO("point " << index);
		CHECK(left->Points[index].X == right->Points[index].X);
		CHECK(left->Points[index].Y == right->Points[index].Y);
		CHECK(left->Points[index].Z == right->Points[index].Z);
	}
	CHECK(left->Faces.size() == right->Faces.size());
	CHECK(
		served.FindMesh(Name("props/crate.amesh"))->TriangleCount() ==
		delivered.FindMesh(Name("props/crate.amesh"))->TriangleCount()
	);
}

TEST_CASE("one unusable entry in a store costs only that entry", "[game][collision]") {
	// **Not fatal**, for the reason a server runs every script even when one
	// fails: refusing the whole world over one model nobody is standing on is
	// worse than hosting the rest of it. A store published without baking its
	// models holds entries `Mesh::Read` cannot use, and one missing chunk is a
	// store somebody has pruned.
	Tree tree;
	auto opened = ChunkStore::Open(tree.Root, true);
	REQUIRE(opened.has_value());

	// **Different bytes for the absent one, because the store is content
	// addressed.** Announcing a second copy of the crate's bytes would name a
	// chunk the crate had already written, and the entry would read perfectly.
	MeshData wider = Box();
	for (MeshVertex &vertex : wider.Vertices) {
		vertex.Position[0] *= 3.0f;
	}
	wider.ComputeBounds();

	Manifest manifest;
	Publish(*opened, manifest, "props/broken.amesh", AssetKind::Mesh, Bytes("not a mesh"));
	Announce(manifest, "props/absent.amesh", AssetKind::Mesh, Encoded(wider));
	Publish(*opened, manifest, "props/crate.amesh", AssetKind::Mesh, Encoded(Box()));

	CollisionShapes shapes;
	CHECK(AddCollisionShapesFrom(shapes, *opened, manifest) == 1);
	CHECK(shapes.FindHull(Name("props/crate.amesh")) != nullptr);
	CHECK(shapes.FindHull(Name("props/broken.amesh")) == nullptr);
	CHECK(shapes.FindHull(Name("props/absent.amesh")) == nullptr);
}

TEST_CASE("merging into a world keeps what the world already had", "[game][collision]") {
	// **Read-modify-write, because `SetResource` replaces.** A world that lost
	// its terrain the moment a crate streamed in would read as the terrain
	// having no collision at all.
	engine::ecs::Store store("collision.merge");

	CollisionShapes terrain;
	AddCollisionShapes(terrain, Name("world/terrain.amesh"), Box());
	MergeCollisionShapes(store, terrain);

	CollisionShapes crate;
	AddCollisionShapes(crate, Name("props/crate.amesh"), Box());
	MergeCollisionShapes(store, crate);

	const CollisionShapes *held = CollisionShapesOf(store);
	REQUIRE(held != nullptr);
	CHECK(held->FindHull(Name("world/terrain.amesh")) != nullptr);
	CHECK(held->FindHull(Name("props/crate.amesh")) != nullptr);
}

TEST_CASE("a world gets the built-ins before any content exists", "[game][collision]") {
	// The one call every host makes, whatever else it has.
	engine::ecs::Store store("collision.builtins");
	RecordBuiltinCollisionShapes(store);

	const CollisionShapes *held = CollisionShapesOf(store);
	REQUIRE(held != nullptr);
	CHECK(held->FindHull(Name(engine::assets::BuiltinName(engine::assets::BuiltinMesh::Cube))) != nullptr);
}

TEST_CASE("a world holding baked shapes can still be snapshotted", "[game][collision]") {
	// **The `client::DrawList` trap, one resource along.** `Store::Save` refuses
	// a resource with no serialisation rather than writing bytes that cannot be
	// read back, and the studio snapshots a universe every time Play is pressed
	// - so the moment every world got collision shapes, every snapshot failed.
	// `scene::RegisterSceneComponents` gives the table an empty writer.
	//
	// **And what comes back is empty**, because a restored world is handed its
	// shapes again by whichever host restored it.
	engine::scene::RegisterSceneComponents();

	engine::ecs::Store store("collision.snapshot");
	RecordBuiltinCollisionShapes(store);
	REQUIRE(CollisionShapesOf(store) != nullptr);
	REQUIRE(CollisionShapesOf(store)->HullCount() != 0);

	engine::core::ByteWriter writer;
	REQUIRE(store.Save(writer));

	engine::ecs::Store restored("collision.snapshot.back");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	const CollisionShapes *held = CollisionShapesOf(restored);
	if (held != nullptr) {
		CHECK(held->HullCount() == 0);
		CHECK(held->MeshCount() == 0);
	}

	// The host puts them back, which is what every restore path does.
	RecordBuiltinCollisionShapes(restored);
	CHECK(CollisionShapesOf(restored)->HullCount() == engine::assets::BUILTIN_MESH_COUNT);
}

TEST_CASE("merging two tables keeps both, and the newer name wins", "[game][collision]") {
	CollisionShapes session;
	AddCollisionShapes(session, Name("props/crate.amesh"), Box());

	MeshData wider = Box();
	for (MeshVertex &vertex : wider.Vertices) {
		vertex.Position[0] *= 4.0f;
	}
	wider.ComputeBounds();

	// A republish of the same name: the store's answer replaces, it does not
	// accumulate a second row.
	CollisionShapes republished;
	AddCollisionShapes(republished, Name("props/crate.amesh"), wider);
	AddCollisionShapes(republished, Name("props/barrel.amesh"), Box());

	MergeCollisionShapes(session, republished);

	CHECK(session.HullCount() == 2);
	const auto *hull = session.FindHull(Name("props/crate.amesh"));
	REQUIRE(hull != nullptr);
	CHECK(hull->Bounds.Maximum.X > 3.0f);
}
