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
#include <cstdint>
#include <span>
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
			// What the authored shape is called. The key the linear search
			// below compares.
			core::Name Name;

			// The hull itself.
			collision::ConvexHull Shape;
		};

		// One named triangle mesh.
		struct MeshRow {
			// What the authored shape is called.
			core::Name Name;

			// The mesh itself.
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

		// Drops both shapes registered under a name, if any are.
		//
		// **Because a shape can now outlive its author.** Every filler before
		// v0.19 was content: a manifest is walked once, and a name registered
		// from it is wanted for as long as the world is. A script that builds
		// geometry at run time is the other case - a terrain streamer creates
		// and destroys a mesh per chunk as somebody walks - and a table that
		// only ever grew would hold a hull and a triangle soup per chunk ever
		// built, none of which anything can name again.
		//
		// Silent when the name is not there, which is the ordinary case for a
		// mesh that never had a collider.
		//
		// @param name The shape to forget.
		// @since v0.19
		void Forget(core::Name name);

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

	// Bakes a hull and a triangle mesh from one set of points and indices.
	//
	// **The one place either shape is built**, which is why it is here rather
	// than beside a caller: `game::AddCollisionShapes` bakes a delivered mesh
	// and `RefreshEditableMeshCollision` bakes one a script built this frame,
	// and a second copy of "quickhull the points, then soup the triangles" is
	// two places for the two paths to drift apart on tolerance or winding.
	//
	// Both, rather than whichever the collider asks for. A `Collider` names its
	// geometry and picks its `ShapeKind` separately, and the two are edited by
	// different people at different times - so baking only the kind that
	// happens to be selected would make switching a part from `Hull` to `Mesh`
	// depend on whether anything else in the world had already asked for one.
	//
	// Nothing is registered for an empty point list: see `game::
	// AddCollisionShapes`, which refuses the same case for the same reason - an
	// empty hull is a collider that is a single point at the part's origin,
	// which stops nothing and says nothing.
	//
	// @param into    The table to write into.
	// @param name    What a `Collider::Geometry` will say.
	// @param points  The vertices, in the part's own object space.
	// @param indices Three per triangle, into `points`.
	// @since v0.19
	void BakeCollisionShapes(
		CollisionShapes &into,
		core::Name name,
		std::span<const core::Vector3> points,
		std::span<const uint32_t> indices
	);

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
