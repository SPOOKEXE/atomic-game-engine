#pragma once

// One drawable thing, as the *world* describes it.
//
// This is the payload a world publishes on `world::ViewChannel`, and it
// replaces `render::Instance` as the thing that crosses from simulation to
// presentation. The two are not the same type wearing different names:
//
// | | `scene::DrawInstance` | `render::Instance` |
// |---|---|---|
// | Says | a cube of oak, here | a `mat4` and an RGBA |
// | Tier | `shared` | `client` |
// | Written by | whoever ticks the world | the renderer's own upload |
//
// **It has to be `shared`, and that is the whole reason it is here.** A
// server-tier host produces one — a headless world still has a draw list, and
// `world::ViewChannel` is how a hosted world's view reaches a client — while a
// client-tier consumer reads it. A type only one of those tiers can name cannot
// be the thing they hand between them.
//
// So it carries **scene data, never device data**: a `CFrame` rather than a
// column-major `mat4`, a `core::Name` rather than a mesh handle, a `Color3`
// rather than a packed RGBA. The conversion to whatever a GPU wants belongs in
// the presentation module, once, at the point of upload — putting it here would
// put a device's memory layout in the type a headless server writes.
//
// `HalfExtent` is the one field `v02v03v04.md` does not name and it is not
// optional: a `CFrame` carries no scale on purpose, and the `mat4` this
// replaces carried it. Without it a two-metre cube and a one-metre cube are the
// same draw instance, and the demo this has to be able to publish scales
// cubes.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::scene {

	// One thing to draw, flat and copyable.
	//
	// Flat because the day a world is a process this has to survive being
	// memcpy'd, so no field of it may be a pointer or own an allocation.
	//
	// Interpolation has already happened by the time one of these exists: this
	// is where the thing *is* for the frame being drawn, not where it was at a
	// tick boundary. A consumer that re-interpolated would be interpolating
	// twice.
	//
	// @since v0.4
	struct DrawInstance {
		// World-space placement for this frame.
		core::CFrame Frame;

		// How big it is, as a half-extent on each local axis — the same form
		// `Bounds` carries, so the producer copies rather than converts.
		core::Vector3 HalfExtent{0.5f, 0.5f, 0.5f};

		// Flat multiplier over the material.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};

		// Which mesh, by name. Invalid means the consumer's default.
		core::Name Mesh;

		// Which material, by name. Invalid means the consumer's default.
		core::Name Material;

		// How much of what is behind shows through, 0 to 1.
		//
		// **The field is cheap and the ordering is not.** A non-zero value puts
		// this instance in a second pass, sorted back-to-front per view — which
		// is the first thing the renderer does that depends on *which camera is
		// looking*, and the reason this arrived with the pass rather than ahead
		// of it. See `SortForDrawing`.
		float Transparency = 0.0f;

		// Which surface texture this instance shows, or -1 for none.
		//
		// **A mirror, and nothing more general than that.** A surface camera
		// renders the world into a texture and an instance carrying its index
		// samples it with a planar projection from that camera — which is
		// exactly right for a flat pane and exactly wrong for anything curved.
		// The narrowness is the design: a general reflection needs a cube map
		// or a screen-space trace, and neither belongs in a pipeline this size.
		int8_t Surface = -1;

		// Whether this instance is drawn into the shadow map.
		//
		// **Carried rather than derived, because the renderer cannot work it
		// out.** Transparency it can — a blended fragment must not write full
		// depth — but "this opaque thing should not occlude" is an authoring
		// decision that exists nowhere in the geometry. See
		// `Visual::CastShadow`, which is where it is authored.
		bool CastShadow = true;

		// Explicit padding, for the reason every other `Reserved` in the engine
		// exists: this type crosses as its object representation the day a world
		// is a process, and uninitialised bytes make two runs of one scene
		// produce different files.
		//
		// Two now, not three. `CastShadow` took one of them, and the type is the
		// size it was.
		uint8_t Reserved[2] = {};
	};

	// Produces the order a draw list should be submitted in.
	//
	// **An order rather than a sort in place**, because the consumer holds a
	// `std::span<const DrawInstance>` — a view published by a world it does not
	// own, which may be another process's memory. Writing an index list also
	// costs four bytes an instance instead of moving eighty.
	//
	// **Why the renderer cannot just draw them in any order.** Opaque geometry
	// writes depth, so whatever is nearest wins whichever order it arrived in.
	// A blended fragment does not replace what is behind it — it mixes with
	// whatever is already in the target — so drawing a near pane before a far
	// one blends the far one *into* a pixel that should have hidden it. The
	// result is a window that looks right from one side of the room and wrong
	// from the other, which reads as a shader bug rather than an ordering one.
	//
	// **Back to front, by squared distance from the eye.** The square root is
	// not taken: it is monotonic, so it cannot change an ordering, and this runs
	// over every transparent instance every frame per view.
	//
	// **A stable sort**, so two panes at the same distance keep the order the
	// world produced them in. An unstable one would swap them from frame to
	// frame as the comparison fell either way, and a recording would stop
	// replaying — which is a determinism failure arriving through a renderer.
	//
	// Here rather than in `render` because it is arithmetic over a `shared`
	// type, and a headless host publishing a view has the same reason to order
	// it. `render` is where the *pipeline* lives; this is where the list does.
	//
	// @param instances The draw list.
	// @param eye       Where the view is, in world space.
	// @param order     Filled in with indices into `instances`. Cleared first.
	// @return How many indices at the front of `order` name opaque instances.
	size_t OrderForDrawing(
		std::span<const DrawInstance> instances, const core::Vector3 &eye, std::vector<uint32_t> &order
	);

	// Whether an instance needs the blended pass.
	//
	// **Not `> 0`, and the epsilon is the point.** A `Transparency` of a
	// millionth is visually opaque and costs a sort, a pipeline switch and the
	// loss of depth writes; a value that small is arithmetic noise from a tween
	// rather than an author's intent.
	//
	// @param instance The instance to classify.
	// @return `true` when it belongs in the transparent pass.
	bool IsTransparent(const DrawInstance &instance);

	// Moves the shadow casters to the front of an order, in place.
	//
	// **A run of one order rather than the whole of it**, because the renderer
	// has already divided the opaque head into what a mirror may see and what it
	// may not — and one partition cannot serve both passes. The surface pass
	// wants the non-surface instances contiguous from zero; the shadow pass
	// wants the casters contiguous. So this is applied to each of those runs
	// separately and the shadow pass draws two ranges, the second of which is
	// empty in every scene with no mirror in it.
	//
	// **Stable**, for the reason every ordering in this file is: an opaque scene
	// must come out exactly as it went in, or a recording stops replaying.
	//
	// Here rather than in `render` for `OrderForDrawing`'s reason — it is
	// arithmetic over a `shared` type, and it is the piece of the shadow pass
	// that can be checked without a GPU. The pass itself is index arithmetic
	// over what this returns, which is exactly the part that is easy to get
	// wrong by one and impossible to see in a screenshot.
	//
	// Only meaningful over opaque instances. A blended fragment writing full
	// depth would cast a solid shadow, so the renderer never offers this the
	// transparent tail — this does not re-check that, because a caller that
	// passed the tail has already made a different mistake.
	//
	// @param instances The draw list the indices refer to.
	// @param order     Indices into `instances`, reordered in place.
	// @return How many at the front of `order` cast a shadow.
	size_t PartitionCasters(std::span<const DrawInstance> instances, std::span<uint32_t> order);

	// Moves the instances that show a surface to the back of an order, in place.
	//
	// **The twin of `PartitionCasters`, and it exists because there were four
	// copies of it.** The same `stable_partition` on `Surface < 0` was written
	// inline in `OrderScene` twice and in the renderer twice, and the copies had
	// already drifted: two spelled the out-of-range guard `index >= size() ||
	// Surface < 0` and two spelled it `index < size() && Surface < 0`, which put
	// a bad index in opposite runs. One helper is what stops that being a thing
	// anyone has to notice.
	//
	// **Stable**, so a run that was sorted back-to-front stays sorted inside each
	// half — the mirrors keep their depth order and so does everything else.
	//
	// **Returns without touching the order when nothing shows a surface**, which
	// is every scene with no mirror in it. `stable_partition` allocates a
	// temporary buffer, so the scan that avoids it is not a micro-optimisation:
	// it is the difference between a per-frame heap allocation in the render path
	// and none.
	//
	// @param instances The draw list the indices refer to.
	// @param order     Indices into `instances`, reordered in place.
	// @return How many at the *back* of `order` show a surface.
	size_t PartitionSurfaces(std::span<const DrawInstance> instances, std::span<uint32_t> order);

	// Where each pass over the scene range starts, and how long it is.
	//
	// **The index arithmetic three passes share, in one place that can be
	// checked without a GPU.** Each field is an offset or a count into the order
	// `OrderScene` produced, and every one of them is a `first_instance` and a
	// count handed to a draw call. That is the part of a render pass that is
	// easiest to get wrong by one and impossible to see in a screenshot: a
	// shadow range short by one loses a caster somewhere off screen, and the
	// frame looks fine.
	//
	// The runs, in the order they sit in the buffer:
	//
	//     [0,                 ReflectedCasters)  opaque, no mirror, casts
	//     [ReflectedCasters,  Reflected)         opaque, no mirror, no shadow
	//     [Reflected,         Reflected + SurfaceCasters)  mirror, casts
	//     [Reflected + SurfaceCasters, Opaque)   mirror, no shadow
	//     [Opaque,            Opaque + Transparent)  blended, far to near
	//
	// @since v0.7
	struct ScenePlan {
		// How many at the front of the order are opaque. The blended tail is
		// everything after it.
		uint32_t Opaque = 0;

		// How many are blended, sorted back to front from the eye.
		uint32_t Transparent = 0;

		// Opaque instances that are *not* mirrors, contiguous from zero.
		//
		// What the surface pass draws: a mirror must not appear in its own
		// reflection, because it sits between its reflection camera and the
		// world and would fill the texture with itself.
		uint32_t Reflected = 0;

		// Opaque mirrors, sitting at the back of the opaque head.
		uint32_t Surfaces = 0;

		// Shadow casters among `Reflected`, contiguous from zero.
		uint32_t ReflectedCasters = 0;

		// Blended instances that show a surface, contiguous at the very end.
		//
		// **The last run of the whole list, so a faded mirror still reflects.**
		// A part with a surface used to leave the opaque head the moment its
		// `Transparency` went above zero — which is where the mirror flag is set
		// — so the reflection did not dim, it disappeared, and the pane fell back
		// to its own tint. That reads as the surface camera having stopped rather
		// than as an ordering rule.
		//
		// They sort back-to-front among themselves and are drawn after every
		// other blended instance, which is the "always draws on top" the feature
		// asks for. **Across the two runs the depth order is therefore not
		// strictly back-to-front**: a blended pane in front of a mirror is drawn
		// before it. Stated rather than hidden, and the trade is deliberate —
		// one sorted run per flag is what lets the mirror flag be a uniform
		// instead of a per-fragment branch on data the shader does not have.
		uint32_t TransparentSurfaces = 0;

		// Shadow casters among `Surfaces`, contiguous from `Reflected`.
		//
		// **The reason the shadow pass draws two ranges and not one.** The
		// surface pass needs the non-mirrors contiguous from zero and the shadow
		// pass needs the casters contiguous; one partition cannot give both, so
		// the casters are partitioned within each run and the mirror half is
		// reached separately. It is zero in every scene with no mirror in it.
		uint32_t SurfaceCasters = 0;
	};

	// Divides one view's draw list into the runs its passes submit.
	//
	// Orders the list — opaque in world order, blended back to front from `eye`
	// — then moves mirrors to the back of the opaque head, then moves shadow
	// casters to the front of each of the two runs that leaves.
	//
	// **Here rather than in `render` for `OrderForDrawing`'s reason**, and with
	// more force: this is where the counts a draw call is given come from, and a
	// renderer is the one module a headless suite cannot exercise. Keeping the
	// arithmetic in `shared` is what lets it be wrong in a test rather than on
	// somebody's screen.
	//
	// @param instances The draw list.
	// @param eye       Where the view is, for the blended sort.
	// @param order     Filled with indices into `instances`. Resized first.
	// @return Where each pass starts and how long it is.
	ScenePlan OrderScene(
		std::span<const DrawInstance> instances, const core::Vector3 &eye, std::vector<uint32_t> &order
	);
}
