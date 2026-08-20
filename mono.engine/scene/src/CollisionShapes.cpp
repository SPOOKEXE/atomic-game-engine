#include <engine/ecs/Store.hpp>
#include <engine/scene/CollisionShapes.hpp>

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

	const CollisionShapes *CollisionShapesOf(const ecs::Store &store) {
		return store.Resource<CollisionShapes>();
	}
}
