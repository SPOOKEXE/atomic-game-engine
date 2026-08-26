#include <engine/ecs/Store.hpp>
#include <engine/scene/CollisionShapes.hpp>

#include <cstddef>
#include <utility>

namespace engine::scene {

	namespace {
		// Replaces the row under `name`, or appends one.
		//
		// Written once as a template because the two tables differ only in what
		// they hold, and two copies of a replace-or-append are two places for a
		// duplicate row to creep in.
		template <class Row, class Shape> void Place(std::vector<Row> &rows, core::Name name, Shape shape) {
			for (Row &row : rows) {
				if (row.Name == name) {
					row.Shape = std::move(shape);
					return;
				}
			}
			rows.push_back(Row{name, std::move(shape)});
		}

		template <class Row> const auto *Look(const std::vector<Row> &rows, core::Name name) {
			// **An invalid name matches nothing**, which is `SurfaceTable::Find`'s
			// first line and the same trap: a collider that never named a shape
			// would otherwise resolve to whatever was registered first without
			// one.
			if (!name.IsValid()) {
				return decltype(&rows.front().Shape){nullptr};
			}

			for (const Row &row : rows) {
				if (row.Name == name) {
					return &row.Shape;
				}
			}
			return decltype(&rows.front().Shape){nullptr};
		}
	}

	void CollisionShapes::SetHull(core::Name name, collision::ConvexHull shape) {
		Place(Hulls, name, std::move(shape));
	}

	void CollisionShapes::SetMesh(core::Name name, collision::TriangleMesh shape) {
		Place(Meshes, name, std::move(shape));
	}

	const collision::ConvexHull *CollisionShapes::FindHull(core::Name name) const {
		if (Hulls.empty()) {
			return nullptr;
		}
		return Look(Hulls, name);
	}

	const collision::TriangleMesh *CollisionShapes::FindMesh(core::Name name) const {
		if (Meshes.empty()) {
			return nullptr;
		}
		return Look(Meshes, name);
	}

	void CollisionShapes::Forget(core::Name name) {
		if (!name.IsValid()) {
			return;
		}

		const auto drop = [name](auto &rows) {
			for (size_t at = 0; at < rows.size(); at++) {
				if (rows[at].Name == name) {
					// Swapped with the last rather than erased in place. The
					// tables are searched linearly and nothing depends on their
					// order, so moving one row beats moving the tail - and a
					// streamer forgetting a chunk a tick is the case this
					// exists for.
					rows[at] = std::move(rows.back());
					rows.pop_back();
					return;
				}
			}
		};

		drop(Hulls);
		drop(Meshes);
	}

	void BakeCollisionShapes(
		CollisionShapes &into,
		core::Name name,
		std::span<const core::Vector3> points,
		std::span<const uint32_t> indices
	) {
		if (!name.IsValid() || points.empty()) {
			return;
		}

		into.SetHull(name, collision::BuildConvexHull(points));
		into.SetMesh(name, collision::BuildTriangleMesh(points, indices));
	}

	const CollisionShapes *CollisionShapesOf(const ecs::Store &store) {
		return store.Resource<CollisionShapes>();
	}
}
