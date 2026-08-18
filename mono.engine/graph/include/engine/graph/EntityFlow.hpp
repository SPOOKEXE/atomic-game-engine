#pragma once

// Which instances a pass draws, as something that flows along a wire.
//
// **The half of a frame that was never in the graph.** `RenderGraph` described
// what was drawn *into* - colour, depth, the shadow atlas - and said nothing at
// all about what was drawn. Culling, ordering and the surface partition were a
// fixed sequence inside `Renderer::Render`, so the one thing a pipeline could
// not say was *which* geometry a pass should take. A pipeline that can add a
// pass but not choose its contents is a pipeline that can only reorder the frame
// somebody else wrote.
//
// So a list of instances is a resource - `ResourceKind::Entities` - and the
// operations on it are nodes:
//
//     entities ──▶ cull-frustum ──▶ order-draw ──▶ opaque ──▶ output
//                       │
//                  filter-tag ──▶ shadow
//
// **Indices, not pointers, and the difference matters.** A list is offsets into
// the view's own draw list, so it stays valid when the caller's span does, it
// costs four bytes an entry, and two lists of the same geometry share nothing
// that can go stale. "Pointers" is the right idea and the wrong representation.
//
// **No device, no GPU, no allocation per frame.** Every operation here is
// arithmetic over spans, which is what lets the whole of it be exercised on a
// machine with nothing to draw with - the same reason `Cull` and `Shadow` are in
// this module rather than in `render`.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace engine::graph {

	// One named list of instances, as it travels between nodes.
	//
	// @since v0.11
	struct EntityList {
		// Which resource this is the contents of.
		core::Name Name;

		// Offsets into the view's draw list, in the order a pass should submit
		// them.
		//
		// **The order is part of the value.** A blended pass wants back to
		// front and a shadow pass does not care; an `order-draw` node is what
		// puts one into the other's list, and a pass that submits in a different
		// order than it was handed would be doing the sorting twice.
		std::vector<uint32_t> Indices;
	};

	// Where the lists live while a view's nodes run.
	//
	// **One per view, cleared between them**, because an index means nothing
	// without the draw list it indexes. Two views of one world have the same
	// geometry and different frustums, so they have different lists.
	//
	// @since v0.11
	class EntityFlow {
	  public:
		// Forgets every list. Called when a view begins.
		void Clear();

		// Replaces a named list.
		//
		// @param name    Which resource.
		// @param indices What is in it. Copied.
		void Set(core::Name name, std::span<const uint32_t> indices);

		// Hands over a list to be filled in place, avoiding a copy.
		//
		// @param name Which resource.
		// @return The list's storage, cleared. Valid until the next `Set` or
		//         `Clear`.
		std::vector<uint32_t> &Open(core::Name name);

		// What is in a named list.
		//
		// @param name Which resource.
		// @return Its contents, or empty when nothing has written it. **Empty
		//         and absent are the same answer on purpose**: a pass reading a
		//         list nothing filled draws nothing, which is what makes a
		//         mis-wired pipeline a black frame rather than a full one.
		std::span<const uint32_t> Get(core::Name name) const;

		// Whether anything has written this list.
		//
		// **Distinct from `Get` being empty**, for a diagnostic rather than for
		// a pass: "nothing filled this" and "the filter rejected everything" are
		// different faults with the same picture.
		bool Has(core::Name name) const;

		// How many lists this view has written.
		size_t Count() const {
			return Live;
		}

	  private:
		// **Kept across views and emptied rather than freed.** This runs once
		// per view per frame and the vectors are the only allocation in the
		// whole file; a steady scene settles to zero of them.
		std::vector<EntityList> Lists;

		// How many of `Lists` this view has written. The rest are last view's
		// storage, waiting to be reused.
		size_t Live = 0;
	};

	// Where something looks from, and through what lens.
	//
	// **The eye is one viewpoint among several.** A shadow pass looks from the
	// light, a mirror from its own camera, and a frustum cull is only "cull
	// against the camera" because nothing could name any other one. This is the
	// value that lets a pipeline say which.
	//
	// @since v0.11
	struct Viewpoint {
		// Where it is and which way it faces.
		core::CFrame Frame;

		// What it sees through. Ignored when `Projection` is set - a light's
		// fitted orthographic box is a matrix and not a lens.
		scene::Camera Lens;

		// A projection somebody already worked out, or an identity.
		//
		// **`FitDirectionalLight` produces one of these and no lens**: the box
		// it fits depends on the scene bound, so there is no field of view to
		// describe it with. A viewpoint carries whichever of the two it has.
		glm::mat4 Projection{1.0f};

		// Whether `Projection` is what to use.
		bool Fitted = false;
	};

	// Named viewpoints, for as long as a view's nodes run.
	//
	// Same shape and same lifetime as `EntityFlow`, and separate from it because
	// they are different sorts of value - a list of what to draw and a place to
	// draw it from.
	//
	// @since v0.11
	class Viewpoints {
	  public:
		// Forgets everything. Called when a view begins.
		void Clear();

		// Names a viewpoint.
		void Set(core::Name name, const Viewpoint &viewpoint);

		// What a name refers to.
		//
		// @param name Which viewpoint.
		// @param out  Filled when it exists.
		// @return Whether it exists. **False rather than a default**, because a
		//         node given a camera wire that leads nowhere should fall back
		//         to the view's own rather than to the origin looking down.
		bool Get(core::Name name, Viewpoint &out) const;

		// How many this view has named.
		size_t Count() const {
			return Live;
		}

	  private:
		struct Entry {
			core::Name Name;
			Viewpoint Value;
		};

		std::vector<Entry> Entries;
		size_t Live = 0;
	};

	// The view-projection a viewpoint sees through.
	//
	// @param viewpoint Where from.
	// @param aspect    The target's shape, for a lens. Ignored when the
	//                  viewpoint carries a fitted projection.
	// @return The matrix a frustum is built from.
	glm::mat4 ViewProjectionOf(const Viewpoint &viewpoint, float aspect);

	// Every instance, in the order the caller gave them.
	//
	// The source node: what `entities` produces before anything has filtered it.
	//
	// @param count How many instances the view has.
	// @param into  Filled with `0..count`.
	void AllEntities(size_t count, std::vector<uint32_t> &into);

	// Those of a list that survive a frustum.
	//
	// **A filter over a list rather than over the instances**, which is what
	// makes it composable: culling the survivors of a tag filter is the same
	// function again, and neither needs to know what the other did.
	//
	// @param instances The view's draw list.
	// @param from      Which of them to consider.
	// @param frustum   What to test against.
	// @param into      The survivors, in `from`'s order. May alias `from`'s
	//                  storage only if the caller passes a different vector.
	// @return How many survived.
	size_t FilterByFrustum(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		const Frustum &frustum,
		std::vector<uint32_t> &into
	);

	// Those of a list whose tag mask overlaps a filter.
	//
	// @param instances The view's draw list.
	// @param from      Which of them to consider.
	// @param mask      Bits to match. Zero matches everything, which is what an
	//                  unset filter means rather than a filter that rejects
	//                  everything.
	// @param into      The survivors, in `from`'s order.
	// @return How many survived.
	size_t FilterByTag(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		uint32_t mask,
		std::vector<uint32_t> &into
	);

	// Those of a list within a radius of a point.
	//
	// @param instances The view's draw list.
	// @param from      Which of them to consider.
	// @param eye       Where to measure from.
	// @param radius    How far is far enough. Non-positive keeps everything.
	// @param into      The survivors, in `from`'s order.
	// @return How many survived.
	size_t FilterByDistance(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		const core::Vector3 &eye,
		float radius,
		std::vector<uint32_t> &into
	);

	// A list reordered for drawing: opaque first, then blended back to front.
	//
	// **`scene::OrderForDrawing` over a subset**, and not a second copy of it.
	// The sort that decides what a transparent pane looks like is written once;
	// this is the node that applies it to whatever the filters left.
	//
	// @param instances The view's draw list.
	// @param from      Which of them to order.
	// @param eye       Where the camera is, for the blended sort.
	// @param into      The ordered list.
	// @return How many of the result are opaque, so a caller can split the runs.
	size_t OrderEntities(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> from,
		const core::Vector3 &eye,
		std::vector<uint32_t> &into
	);

	// What one graph node changed in the entity flow.
	//
	// @since v0.17
	struct EntityNodeRun {
		// False when the node is not an entity or camera operation.
		bool Handled = false;

		// True only for `order-draw`, whose opaque prefix is meaningful to a
		// geometry backend.
		bool Ordered = false;

		// The entity resource written, invalid for a camera operation.
		core::Name Output;

		// Output entries and the opaque prefix after an ordering operation.
		//@{
		size_t Count = 0;
		size_t Opaque = 0;
		//@}
	};

	// Executes one device-free camera or entity node.
	//
	// The graph supplies resource kinds, the node supplies its parameters and
	// the two flow stores retain list capacity between views. An input may equal
	// its output; only that case takes a protective copy before opening the
	// destination.
	//
	// @param graph       The graph that owns the node's resource ids.
	// @param node        The operation to execute.
	// @param instances   The view's complete draw list.
	// @param fallback    The view camera used by an unwired camera input.
	// @param aspect      Target width divided by height.
	// @param entities    Named entity lists for this view.
	// @param viewpoints  Named viewpoints for this view.
	// @return What was handled and produced.
	EntityNodeRun RunEntityNode(
		const RenderGraph &graph,
		const Node &node,
		std::span<const scene::DrawInstance> instances,
		const Viewpoint &fallback,
		float aspect,
		EntityFlow &entities,
		Viewpoints &viewpoints
	);
}
