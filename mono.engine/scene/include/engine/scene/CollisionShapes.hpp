#pragma once

// The baked shapes a `Collider` names, resolved once per world.
//
// **`scene::SurfaceTable`'s arrangement, one layer of indirection further.** A
// `Collider` names its geometry with a string because a name is what survives a
// save file, a wire format and a rename - rule 4 of the root `AGENTS.md`. The
// bytes behind that name are a hull or a triangle soup, and they are the same
// bytes on every part that uses them, so they live in a resource and the loop
// reads them once.
//
// **Why the table is here and not in `physics`.** `collision` is L5 and
// `physics` is L8, so physics could hold this - but then a `server`-tier host
// that wanted to *fill* it would have to reach L8 to do so, and the thing that
// fills it is whoever loaded the content. `scene` is where the components that
// name shapes already are, and it is below every host.
//
// **Nothing here reads a file.** A hull arrives as points and a mesh as vertices
// and indices; turning a `.glb` into those is `bake`'s job at L9, and an edge
// from here to it would invert the stack.
//
// @tier L7 · shared
// @since v0.17

#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Name.hpp>

#include <cstddef>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Every baked collision shape one world can name.
	//
	// One per world, as a resource. A world with no mesh colliders in it holds an
	// empty one and costs nothing.
	//
	// @since v0.17
	struct CollisionShapes {
		// One named convex hull.
		struct HullRow {
			core::Name Name;
			collision::ConvexHull Shape;
		};

		// One named triangle mesh.
		struct MeshRow {
			core::Name Name;
			collision::TriangleMesh Shape;
		};

		// **A vector and a linear search, not a map**, which is `SurfaceTable`'s
		// decision and holds here for its reason and one more. The reason: an
		// unordered container's walk order is the allocator's, and
		// `physics/AGENTS.md` refuses one anywhere in a system. The extra one: a
		// world has a handful of distinct collision shapes however many parts use
		// them, because the whole point of naming them is that they are shared -
		// so the search is over single digits and a hash would cost more than it
		// saved.
		//@{
		std::vector<HullRow> Hulls;
		std::vector<MeshRow> Meshes;
		//@}

		// Adds or replaces a hull.
		//
		// **Replaces rather than appends a second row with the same name.** Two
		// rows for one name is a lookup whose answer depends on which was found
		// first, and the case that produces it is a world reloading content it
		// already had.
		//
		// @param name  What a `Collider::Geometry` will say.
		// @param shape The hull, in the part's own object space.
		void SetHull(core::Name name, collision::ConvexHull shape);

		// Adds or replaces a triangle mesh, per `SetHull`.
		//
		// @param name  What a `Collider::Geometry` will say.
		// @param shape The mesh, in the part's own object space.
		void SetMesh(core::Name name, collision::TriangleMesh shape);

		// The hull under a name, or nothing.
		//
		// **There is deliberately no get-or-default**, matching `SurfaceTable`: a
		// caller has to decide what a missing shape means, and for `physics` the
		// answer is "collide as the part's bound" rather than "do not collide",
		// which is a decision a default here would have taken away.
		//
		// @param name The name to resolve.
		// @return A pointer into `Hulls`, invalidated by the next `SetHull`, or
		//         `nullptr`.
		const collision::ConvexHull *FindHull(core::Name name) const;

		// The triangle mesh under a name, or nothing. See `FindHull`.
		//
		// @param name The name to resolve.
		// @return A pointer into `Meshes`, invalidated by the next `SetMesh`, or
		//         `nullptr`.
		const collision::TriangleMesh *FindMesh(core::Name name) const;

		// How many shapes of each kind are registered.
		//@{
		size_t HullCount() const {
			return Hulls.size();
		}

		size_t MeshCount() const {
			return Meshes.size();
		}
		//@}
	};

	// The world's shape table, or nothing.
	//
	// **A free function rather than the resource at each call site**, for
	// `SurfaceTable`'s reason: a world that never registered a shape has no
	// resource at all, and a caller that forgot the null check would dereference
	// nothing on the first world without a mesh collider in it.
	//
	// @param store The world.
	// @return The table, or `nullptr`.
	// @since v0.17
	const CollisionShapes *CollisionShapesOf(const ecs::Store &store);
}
