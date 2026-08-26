#include <engine/assets/Builtin.hpp>
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/CollisionContent.hpp>
#include <engine/scene/CollisionShapes.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace engine::game {

	namespace {
		// A mesh's vertex positions, which is all either shape needs.
		//
		// `assets::MeshVertex` carries a normal and a texture coordinate too,
		// and neither means anything to a collider: a hull is its corners and a
		// soup is its triangles.
		std::vector<core::Vector3> PositionsOf(const assets::MeshData &mesh) {
			std::vector<core::Vector3> points;
			points.reserve(mesh.Vertices.size());
			for (const assets::MeshVertex &vertex : mesh.Vertices) {
				points.push_back(core::Vector3{vertex.Position[0], vertex.Position[1], vertex.Position[2]});
			}
			return points;
		}

		// The world's table, or an empty one. The caller writes it back.
		scene::CollisionShapes Held(ecs::Store &store) {
			if (const scene::CollisionShapes *held = scene::CollisionShapesOf(store)) {
				return *held;
			}
			return scene::CollisionShapes{};
		}
	}

	void AddCollisionShapes(scene::CollisionShapes &into, core::Name name, const assets::MeshData &mesh) {
		if (!name.IsValid()) {
			// An unnamed shape is one nothing can ever ask for, and registering
			// it would put a row in the table that every lookup walks past.
			// `CollisionShapes::FindHull` refuses an invalid name at the other
			// end for the matching reason.
			return;
		}

		if (mesh.Vertices.empty()) {
			// **A mesh with no vertices registers nothing, rather than an empty
			// shape.** `collision::SupportPoint` answers the origin for a hull
			// with no points, so an empty hull is a collider that is a single
			// point at the part's own position - which stops nothing and says
			// nothing. Leaving the name unresolved makes the collider fall back
			// to the part's bound instead, which `ShapeInstance` states as the
			// behaviour for a name that does not resolve.
			return;
		}

		// **Through `scene::BakeCollisionShapes`, which is the one place either
		// shape is built.** A script that builds a mesh at run time bakes the
		// same two out of the same arrays - see
		// `scene::RefreshEditableMeshCollision` - and a second copy of
		// "quickhull the points, then soup the triangles" is two places for the
		// two paths to disagree about tolerance or winding.
		scene::BakeCollisionShapes(into, name, PositionsOf(mesh), mesh.Indices);
	}

	void AddBuiltinCollisionShapes(scene::CollisionShapes &into) {
		// **Baked once per process, not once per call.** The six are generated
		// from a description rather than read, and a studio with four viewports
		// on four worlds would otherwise run quickhull twenty-four times to
		// produce the same six hulls.
		//
		// A function-local static rather than a namespace one, so the work
		// happens on the first thread that asks rather than during static
		// initialisation, where the order between translation units is nobody's
		// to control.
		struct Baked {
			core::Name Name;
			collision::ConvexHull Hull;
			collision::TriangleMesh Mesh;
		};

		static const std::vector<Baked> BUILTINS = [] {
			std::vector<Baked> baked;
			baked.reserve(assets::BUILTIN_MESH_COUNT);

			for (uint8_t index = 0; index < assets::BUILTIN_MESH_COUNT; index++) {
				const auto which = static_cast<assets::BuiltinMesh>(index);
				const assets::MeshData mesh = assets::MakeBuiltin(which);
				const std::vector<core::Vector3> points = PositionsOf(mesh);

				baked.push_back(
					Baked{
						core::Name(assets::BuiltinName(which)),
						collision::BuildConvexHull(points),
						collision::BuildTriangleMesh(points, mesh.Indices),
					}
				);
			}
			return baked;
		}();

		for (const Baked &builtin : BUILTINS) {
			into.SetHull(builtin.Name, builtin.Hull);
			into.SetMesh(builtin.Name, builtin.Mesh);
		}
	}

	size_t AddCollisionShapesFrom(
		scene::CollisionShapes &into, const assets::ChunkStore &chunks, const assets::Manifest &manifest
	) {
		size_t baked = 0;
		for (const assets::AssetEntry &entry : manifest.Assets()) {
			if (entry.Kind != assets::AssetKind::Mesh) {
				continue;
			}

			const core::Name name(entry.Name);
			if (!name.IsValid()) {
				continue;
			}

			// **The whole asset, because a hull needs every vertex.** This is
			// the one pass in the engine that reads a mesh it is not going to
			// draw, and it is a startup cost paid once rather than per world.
			const std::optional<std::vector<std::byte>> bytes = chunks.ReadAsset(entry);
			if (!bytes) {
				ENGINE_WARN("collision: {} is in the manifest and not in the store", entry.Name);
				continue;
			}

			core::ByteReader reader(*bytes);
			assets::MeshData mesh;
			if (!assets::Mesh::Read(reader, mesh)) {
				ENGINE_WARN("collision: {} is not a mesh this engine reads", entry.Name);
				continue;
			}

			AddCollisionShapes(into, name, mesh);
			baked++;
		}
		return baked;
	}

	void MergeCollisionShapes(scene::CollisionShapes &into, const scene::CollisionShapes &from) {
		for (const scene::CollisionShapes::HullRow &row : from.Hulls) {
			into.SetHull(row.Name, row.Shape);
		}
		for (const scene::CollisionShapes::MeshRow &row : from.Meshes) {
			into.SetMesh(row.Name, row.Shape);
		}
	}

	void RecordBuiltinCollisionShapes(ecs::Store &store) {
		scene::CollisionShapes shapes = Held(store);
		AddBuiltinCollisionShapes(shapes);
		store.SetResource(std::move(shapes));
	}

	void MergeCollisionShapes(ecs::Store &store, const scene::CollisionShapes &shapes) {
		scene::CollisionShapes merged = Held(store);
		MergeCollisionShapes(merged, shapes);
		store.SetResource(std::move(merged));
	}
}
