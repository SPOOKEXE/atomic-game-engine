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
// server-tier host produces one - a headless world still has a draw list, and
// `world::ViewChannel` is how a hosted world's view reaches a client - while a
// client-tier consumer reads it. A type only one of those tiers can name cannot
// be the thing they hand between them.
//
// So it carries **scene data, never device data**: a `CFrame` rather than a
// column-major `mat4`, a `core::Name` rather than a mesh handle, a `Color3`
// rather than a packed RGBA. The conversion to whatever a GPU wants belongs in
// the presentation module, once, at the point of upload - putting it here would
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
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>

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

		// How big it is, as a half-extent on each local axis - the same form
		// `Bounds` carries, so the producer copies rather than converts.
		core::Vector3 HalfExtent{0.5f, 0.5f, 0.5f};

		// Flat multiplier over the material.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};
		core::Color3 SurfaceColour{1.0f, 1.0f, 1.0f};
		core::Color3 EmissiveTint{1.0f, 1.0f, 1.0f};
		float EmissiveStrength = 1.0f;

		// Which mesh, by name. Invalid means the consumer's default.
		core::Name Mesh;

		// Which texture overrides whatever the mesh's own submeshes name.
		//
		// **An override rather than the only answer**, and the two-level rule
		// is what makes an imported model work at all: a `.pmx` character has
		// twenty submeshes each naming its own sheet, and no single field on an
		// instance can say that. So the mesh carries a texture per run and this
		// replaces it for the whole instance when it is set - which is exactly
		// what Roblox's `MeshPart.TextureID` does to an imported mesh, and
		// exactly what a recoloured variant of one model needs.
		//
		// From `SurfaceAppearance::ColourMap`. Invalid means the submeshes
		// decide.
		core::Name Texture;

		// The surface's other sampled maps, from `SurfaceAppearance`.
		//
		// Invalid means the shader's fallback - the geometric normal, and
		// constants for roughness, occlusion, height and emission.
		//@{
		core::Name NormalMap;
		core::Name RoughnessMap;
		core::Name OcclusionMap;
		core::Name HeightMap;
		core::Name MetalnessMap;
		core::Name EmissiveMap;
		//@}

		// Which shader draws this instance, or invalid for the engine's own.
		//
		// **A name and not a pipeline handle**, which is the same rule `Mesh`
		// and `Texture` follow and the same reason: a `server`-tier host writes
		// this and a device handle means nothing to one. `render` resolves it
		// against what `ShaderLibrary` compiled, exactly as it resolves a mesh
		// name against `MeshTable`.
		//
		// **Invalid is the ordinary state and costs one compare.** The draw loop
		// breaks a run wherever this changes, so a world where nothing selects a
		// shader holds one value throughout and never breaks on it - the rule
		// `SeamNormal` already established for a per-instance fact expressed as
		// a per-draw one.
		//
		// From `SurfaceAppearance::Shader`, which `ResolveMaterials` wrote from
		// the `Material` child.
		//
		// @since v0.15
		core::Name Shader;

		// Which tag bits this instance carries, against the world's `TagTable`.
		//
		// **The mask travels rather than the names**, so a pass filtering by
		// tag is an `and` per instance instead of a set lookup - see
		// `MatchesTags`. It is on the instance rather than looked up per view
		// because a view is drawn several times a frame and the world is walked
		// once.
		uint32_t TagMask = 0;

		// How much of what is behind shows through, 0 to 1.
		//
		// **The field is cheap and the ordering is not.** A non-zero value puts
		// this instance in a second pass, sorted back-to-front per view - which
		// is the first thing the renderer does that depends on *which camera is
		// looking*, and the reason this arrived with the pass rather than ahead
		// of it. See `SortForDrawing`.
		float Transparency = 0.0f;

		// The texture alpha threshold used by `AlphaMode::Transparency` when the
		// part itself is opaque.
		float AlphaCutoff = 0.5f;

		// Which surface texture this instance shows, or -1 for none.
		//
		// **A mirror, and nothing more general than that.** A surface camera
		// renders the world into a texture and an instance carrying its index
		// samples it with a planar projection from that camera - which is
		// exactly right for a flat pane and exactly wrong for anything curved.
		// The narrowness is the design: a general reflection needs a cube map
		// or a screen-space trace, and neither belongs in a pipeline this size.
		//
		// **Sixteen bits since v0.17, and it cost nothing.** It was `int8_t`,
		// which put a hard ceiling of a hundred and twenty-seven mirrors in the
		// smallest field in the engine - a design limit hiding in a type. The
		// three bytes here were followed by one of padding before `SeamNormal`,
		// so widening it fills the hole and `sizeof(DrawInstance)` is unchanged;
		// the `static_assert` below is what keeps that true.
		int16_t Surface = -1;

		// Whether this instance is drawn into the shadow map.
		//
		// **Carried rather than derived, because the renderer cannot work it
		// out.** Transparency it can - a blended fragment must not write full
		// depth - but "this opaque thing should not occlude" is an authoring
		// decision that exists nowhere in the geometry. See
		// `Visual::CastShadow`, which is where it is authored.
		bool CastShadow = true;

		// How this instance's alpha channel is treated.
		//
		// **From `SurfaceAppearance::Mode`, and it is here rather than derived
		// from `Transparency` because they answer different questions.**
		// `Transparency` is how see-through the *part* is and puts it in the
		// sorted pass; this is what the texture's alpha means.
		AlphaMode Alpha = AlphaMode::Opaque;
		SurfaceResampleMode Resample = SurfaceResampleMode::Default;

		// **`Movable` was here and is gone.** It said whether an instance was a
		// thing in the world rather than the world, because the pass that copies
		// a body onto the far side of a hole reads a draw list and could not ask.
		// What replaced it is a better question asked of the same row: whether
		// the body **fits through the hole**, which `scene::CutOfSeam` answers
		// from the box and the pane's own rectangle. A room does not fit through
		// its own doorway and a person does, which is the distinction `Movable`
		// was standing in for - and unlike `Movable` it also admits an anchored
		// crate resting in a seam, which is as much a thing standing in the hole
		// as anything that walked there.
		//
		// `Alpha` and `CastShadow` took the padding bytes before it; `Texture`
		// and `TagMask` did not fit and the type grew by eight for them. The
		// fields below are the addition that widened it again, and the static
		// assert in `tests/DrawInstance.cpp` is what says so out loud.

		// The half-space this instance keeps, as a world plane: the unit normal
		// and the offset along it, keeping `dot(p, SeamNormal) >= SeamOffset`.
		//
		// **A body standing in a portal is one body cut at the plane, not two
		// bodies.** `CutAndCloneSeams` appends a copy of a straddler on the far
		// side of the hole, and without a cut both copies are drawn whole: the
		// original hangs out of the back of the pane into the room it is walking
		// into, and the copy hangs out of the far pane back into the room it came
		// from. A pane set into a thick wall hides both overhangs, which is why
		// that survived three scenes; a free-standing pane shows two crates in a
		// doorway. The near half and the far half meet at the plane with no
		// overlap and no gap, and what fills the missing half of each is the
		// picture in the hole.
		//
		// **A zero normal means whole**, which is every instance in an ordinary
		// scene - the test in `opaque.frag` is behind that check, and the
		// renderer breaks a draw run only where the plane changes, so a world
		// with no portal in it pays nothing at all.
		//
		// **Two fields rather than one four-vector, because `core` has no
		// `Vector4`** and a type the whole engine then has to have an opinion
		// about is a poor price for a plane that lives on one row.
		//
		// @since v0.15
		//@{
		core::Vector3 SeamNormal{0.0f, 0.0f, 0.0f};
		float SeamOffset = 0.0f;
		//@}

		// Which way the sun comes from *for this half*, or zero for the world's.
		//
		// **A copy turned by `R` has to be lit by `R · L`.** The far half of a
		// body standing in a hole is the near half mapped through the seam, so
		// its normals are `R · n` - and shading those with the world's own `L`
		// gives a body whose two halves are lit by two suns that differ by
		// exactly the turn between the panes. On a quarter-turn pair that is a
		// bright face meeting an olive one down the middle of a crate, which
		// reads as the copy being a different object.
		//
		// `R · L` makes `dot(R n, R L)` equal `dot(n, L)` for every normal, so
		// the two halves shade identically and the join is invisible. It is the
		// cheapest half of light through a hole and the only one that needs
		// nothing from the shadow pipeline - `NON-EUCLIDEAN.md` Part V.3.
		//
		// **The ambient and the upward bounce are not turned**, and cannot be
		// without turning the world's up as well. They are the two terms that do
		// not depend on the light's direction, so what is left is a fraction of
		// a tone across the seam rather than a face.
		//
		// @since v0.15
		core::Vector3 SeamLight{0.0f, 0.0f, 0.0f};

		// Which rig this row is a limb of, or zero for a row that is one thing.
		//
		// **A seam has to be able to cut a body once rather than a dozen
		// times.** Whether a row may be cut by a hole is "does it fit the
		// aperture", and a character is a dozen drawn rows: standing in a
		// doorway, its root and its head are comfortably inside the rectangle
		// and its feet sit on the bottom edge, where the box overhangs. Asked
		// per row, that answers yes for most of the rig and no for the parts on
		// the rim - and a row that answers no is drawn *whole* rather than cut,
		// so the body is clipped at the plane from the waist up and pushed
		// through the wall from the ankles down. `CutAndCloneSeams` asks the
		// question once per rig instead, of the box that holds all of it.
		//
		// `ecs::Entity::Id` rather than the handle, so this header keeps its
		// present dependencies. Nothing here dereferences it: it is an identity
		// to group by and the value is never looked up.
		//
		// **Explicit, because an `ecs::Entity` is eight-byte aligned and every
		// field above this one is four.** The compiler would open a four-byte
		// hole here on its own. Named and zeroed, it remains a flat payload with
		// a known object representation, which the module's padding test pins.
		// `CharacterLimb::Reserved` exists for the same reason.
		uint32_t Reserved = 0;

		// @since v0.19
		uint64_t Rig = 0;

		// The stable identity of this drawable inside its source world.
		//
		// **The entity and not the row position.** Culling and ordering permute a
		// draw list every frame, while this value remains attached to the thing
		// being drawn. The renderer uses it to keep one packed GPU row resident
		// and makes visibility a separate index stream.
		uint64_t Source = 0;

		// Which synthetic form of `Source` this row is, or zero for the entity
		// itself. A portal half uses the pane entity, so the original and its copy
		// can both be resident without claiming the same slot.
		uint64_t Variant = 0;

		// The world `Source` belongs to, or invalid for the view's own world.
		//
		// Entity handles collide between stores. A name is the boundary identity
		// this repository permits, and a collector may leave the common case
		// implicit so one world name is written once on the view rather than once
		// per row.
		core::Name SourceWorld;

		// Keeps the flat payload free of implicit tail padding.
		uint32_t IdentityReserved = 0;
	};

	// Fills the fields a collector reads straight off the world's components.
	//
	// **The one place that field list is spelled out.** Two collectors publish
	// this type - `engine::render::CollectInstances` from a world this machine ticks and
	// `client::CollectReplicated` from one it receives - and each wrote its own
	// fourteen-member aggregate until v0.15. That is the most expensive shape a
	// duplicate can take here, because the drift is silent: a member added to the
	// struct above takes its default in whichever collector nobody remembered to
	// edit, so a locally hosted world and a replicated one draw differently and
	// no screenshot says which field went missing. They had already drifted on
	// which components they treat as optional.
	//
	// **What the two genuinely disagree about is deliberately not in here**:
	// which rows they visit, and where the frame comes from. One gates on the
	// `Rendered` mark and interpolates `PreviousTransform` to `Transform` by the
	// frame's alpha; the other gates on `Visual::Visible` and samples a
	// `replication::SnapshotBuffer`. Folding those in would need a flag argument
	// per difference, and a builder with a mode is two builders again.
	//
	// The seam fields are left at their defaults, because they are written by
	// `CutAndCloneSeams` after the list is built rather than read off a row.
	//
	// @param frame      Where to draw, already interpolated by the caller.
	// @param bounds     The row's extent.
	// @param visual     The row's visual component.
	// @param appearance The row's appearance, or null for the defaults - a
	//                   replicated row may arrive without one.
	// @param tags       The row's tags, or null for none.
	// @param source     The entity this row draws, as its complete id.
	// @param local      This viewer's own occlusion fade, or null for none -
	//                   see `scene::LocalTransparency`. Never present on a
	//                   headless host's own draw list, because nothing there
	//                   is looking at anything.
	// @param limb       The row's limb, or null when the row is not part of a
	//                   character. A limb names the root it hangs off and every
	//                   limb of one character names the same root, which is the
	//                   grouping a portal seam needs; a row without one is its
	//                   own body. See `DrawInstance::Rig`.
	// @return The instance to publish.
	// @since v0.15
	inline DrawInstance MakeDrawInstance(
		const core::CFrame &frame,
		const Bounds &bounds,
		const Visual &visual,
		const SurfaceAppearance *appearance,
		const Tags *tags,
		uint64_t source,
		const LocalTransparency *local = nullptr,
		const CharacterLimb *limb = nullptr
	) {
		DrawInstance instance;
		instance.Frame = frame;
		instance.HalfExtent = bounds.HalfExtent;
		instance.Tint = visual.Tint;
		instance.Mesh = visual.Mesh;

		if (appearance != nullptr) {
			instance.SurfaceColour = appearance->Colour;
			instance.EmissiveTint = appearance->EmissiveTint;
			instance.EmissiveStrength = appearance->EmissiveStrength;
			instance.Texture = appearance->ColourMap;
			instance.NormalMap = appearance->NormalMap;
			instance.RoughnessMap = appearance->RoughnessMap;
			instance.OcclusionMap = appearance->OcclusionMap;
			instance.HeightMap = appearance->HeightMap;
			instance.MetalnessMap = appearance->MetalnessMap;
			instance.EmissiveMap = appearance->EmissiveMap;
			instance.Shader = appearance->Shader;
			instance.Alpha = appearance->Mode;
			instance.AlphaCutoff = appearance->AlphaCutoff;
			instance.Resample = appearance->Resample;
		}

		instance.TagMask = tags != nullptr ? tags->Mask : 0u;

		// Copied rather than resolved. Which pass an instance lands in is the
		// renderer's decision, because it depends on where the camera is - and a
		// collector runs once for a world that may be drawn from several views.
		instance.Transparency = visual.Transparency;

		// **The local override wins, and only away from zero.** See
		// `scene::LocalTransparency`'s header for why this is an override
		// rather than a sum, and why it is read here rather than left for the
		// renderer - the renderer never sees `Visual` at all, only the
		// instance this function builds.
		if (local != nullptr && local->Value != 0.0f) {
			instance.Transparency = local->Value;
		}
		// See `DrawInstance::Rig`. A limb names the root it hangs off, and every
		// limb of one character names the same one, which is the grouping a seam
		// needs. A row that is not a limb is its own body and needs no group.
		if (limb != nullptr) {
			instance.Rig = limb->Root.Id;
		}

		instance.Surface = visual.Surface;
		instance.CastShadow = visual.CastShadow;
		instance.Source = source;

		return instance;
	}

	// Produces the order a draw list should be submitted in.
	//
	// **An order rather than a sort in place**, because the consumer holds a
	// `std::span<const DrawInstance>` - a view published by a world it does not
	// own, which may be another process's memory. Writing an index list also
	// costs four bytes an instance instead of moving a wide visual row.
	//
	// **Why the renderer cannot just draw them in any order.** Opaque geometry
	// writes depth, so whatever is nearest wins whichever order it arrived in.
	// A blended fragment does not replace what is behind it - it mixes with
	// whatever is already in the target - so drawing a near pane before a far
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
	// replaying - which is a determinism failure arriving through a renderer.
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

	// The same, over a subset a caller already chose.
	//
	// **What `OrderForDrawing` became when culling moved into the graph.** A
	// pass is handed a list of instances now rather than the whole world - see
	// `graph::EntityFlow` - so the sort has to run over a list. This is that
	// function; `OrderForDrawing` is it applied to everything.
	//
	// An index past the end of `instances` is kept and sorted last rather than
	// dereferenced: a list is whatever a chain of filter nodes produced, and a
	// mis-wired one should lose an object rather than read off the end.
	//
	// @param instances The whole draw list, which the indices are into.
	// @param from      Which of them to order.
	// @param eye       Where the view is, in world space.
	// @param order     Filled with a permutation of `from`. Cleared first.
	// @return How many at the front of `order` name opaque instances.
	size_t OrderSubset(
		std::span<const DrawInstance> instances,
		std::span<const uint32_t> from,
		const core::Vector3 &eye,
		std::vector<uint32_t> &order
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
	// may not - and one partition cannot serve both passes. The surface pass
	// wants the non-surface instances contiguous from zero; the shadow pass
	// wants the casters contiguous. So this is applied to each of those runs
	// separately and the shadow pass draws two ranges, the second of which is
	// empty in every scene with no mirror in it.
	//
	// **Stable**, for the reason every ordering in this file is: an opaque scene
	// must come out exactly as it went in, or a recording stops replaying.
	//
	// Here rather than in `render` for `OrderForDrawing`'s reason - it is
	// arithmetic over a `shared` type, and it is the piece of the shadow pass
	// that can be checked without a GPU. The pass itself is index arithmetic
	// over what this returns, which is exactly the part that is easy to get
	// wrong by one and impossible to see in a screenshot.
	//
	// Only meaningful over opaque instances. A blended fragment writing full
	// depth would cast a solid shadow, so the renderer never offers this the
	// transparent tail - this does not re-check that, because a caller that
	// passed the tail has already made a different mistake.
	//
	// @param instances The draw list the indices refer to.
	// @param order     Indices into `instances`, reordered in place.
	// @return How many at the front of `order` cast a shadow.
	size_t PartitionCasters(std::span<const DrawInstance> instances, std::span<uint32_t> order);

	// Moves the instances that show a surface to the back of an order, in place.
	//
	// **The twin of `PartitionCasters`, and it exists because there were four
	// Keeps the instances whose geometry has arrived.
	//
	// **An instance naming no mesh is kept and one naming an absent mesh is
	// not**, and that distinction is the whole function:
	//
	//   - no mesh named - an ordinary `Part` - draws the renderer's default
	//     cube, which is what a part *is*.
	//   - a mesh named and not loaded - a `MeshPart` whose geometry has not
	//     arrived - draws nothing until it has.
	//
	// Without the second, a mesh table hands back its default for a name it does
	// not hold, and a scene of mesh parts comes up as a field of cubes that turn
	// into models one at a time as content lands. That is worse than empty
	// space: empty space reads as "still loading" and a wrong cube reads as the
	// asset being broken.
	//
	// **Here rather than in the renderer, for `OrderScene`'s reason.** A
	// renderer is the one module a test cannot exercise, so a rule that decides
	// what reaches a draw call is the last place it should live. A template so
	// the residency test is a direct call rather than a `std::function` per
	// instance in the hottest pass of the frame.
	//
	// @param instances What the world produced.
	// @param resident  Called as `resident(const core::Name &)` for each named
	//                  mesh. `true` when the renderer holds it.
	// @param out       Cleared, then filled with what may be drawn.
	// @since v0.12
	template <class Resident>
	void
	KeepLoaded(std::span<const DrawInstance> instances, Resident resident, std::vector<DrawInstance> &out) {
		out.clear();
		out.reserve(instances.size());

		for (const DrawInstance &instance : instances) {
			if (instance.Mesh.IsValid() && !resident(instance.Mesh)) {
				continue;
			}
			out.push_back(instance);
		}
	}

	// copies of it.** The same `stable_partition` on `Surface < 0` was written
	// inline in `OrderScene` twice and in the renderer twice, and the copies had
	// already drifted: two spelled the out-of-range guard `index >= size() ||
	// Surface < 0` and two spelled it `index < size() && Surface < 0`, which put
	// a bad index in opposite runs. One helper is what stops that being a thing
	// anyone has to notice.
	//
	// **Stable**, so a run that was sorted back-to-front stays sorted inside each
	// half - the mirrors keep their depth order and so does everything else.
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

	// How many surface slots anything here has storage for.
	//
	// **A bound on an allocation, not a statement about how many mirrors a world
	// should have.** That second question is the world's, through
	// `workspace.MaxSurfaces` and `scene::SurfaceLimitOf` - which defaults to
	// thirty-two and is what an author actually turns. This number only says how far
	// the arrays sized by it reach, and it exists for the reason
	// `spatial::HashGrid::MAXIMUM_CELLS_PER_PROXY` does: a save file is hostile
	// input, and a world asking for a million mirrors must reach a bound rather
	// than an allocator.
	//
	// **It was sixteen until v0.17 and the reason it moved is that it was being
	// read as the design.** Sixteen was chosen because a room has four walls;
	// what it meant in practice was that a hall of mirrors stopped at sixteen
	// however much memory the device had. The per-world limit is the knob now,
	// and this is headroom above its default.
	//
	// **What raising it costs, so the next person does not have to find out.**
	// Every index in use is two colour targets and a depth buffer, and the
	// descriptors for them are member arrays on the renderer's per-viewport
	// bank - about three hundred bytes a slot, which is why a hundred and
	// twenty-eight is comfortable. Going much past that wants `Renderer.cpp`'s
	// per-frame arrays off the stack first: the surface pass builds a handful of
	// `scene::SurfacePane`-sized arrays this long as locals, and those are the
	// things that would grow, not the textures - which are made on demand and
	// only for the slots a frame actually uses.
	//
	// A scene may name any index it likes; one at or above this is dropped from
	// the view list with a line in the log rather than silently rendering
	// nothing, which is the failure that reads as a broken mirror.
	//
	// @since v0.8
	constexpr uint16_t MAX_SURFACES = 128;

	// Where one surface's instances sit in an ordered draw list.
	//
	// **One run per surface, because a mirror is no longer one texture.** Until
	// v0.8 every pane sampled the same target, so "the mirrors" was a single
	// range and a single sampler binding. With a texture per surface the passes
	// have to bind and project *per index*, which means each index's instances
	// must be contiguous - this is where that contiguity is recorded.
	//
	// Empty runs are the ordinary case: a scene with two mirrors leaves fourteen
	// of these zeroed, and a zero count is a draw call not issued rather than a
	// state to check for.
	//
	// @since v0.8
	struct SurfaceRun {
		// Where this surface's opaque instances start, as an index into the
		// order.
		uint32_t OpaqueFirst = 0;

		// How many there are.
		uint32_t OpaqueCount = 0;

		// Shadow casters among them, contiguous from `OpaqueFirst`.
		//
		// **Partitioned inside the run rather than across all mirrors**, which
		// is the change per-surface grouping forced. One range cannot be both
		// grouped by index and split by caster, and the shadow pass is the one
		// that can afford several draw calls: it draws a handful of mirrors,
		// while the grouping is what every surface pass depends on.
		uint32_t OpaqueCasters = 0;

		// Where this surface's blended instances start.
		uint32_t BlendedFirst = 0;

		// How many there are.
		uint32_t BlendedCount = 0;
	};

	// Groups an already-partitioned run of mirrors by the surface each shows,
	// and records where each index lands.
	//
	// **Exported because there are two ordered lists and only one of them is
	// `OrderScene`'s.** The scene range - what the shadow and surface passes
	// draw - is the whole draw list. The camera range is the frustum-culled
	// survivors, ordered from the eye, and the renderer builds it separately
	// because culling to the eye is exactly what the other two passes must not
	// do. Both need their mirrors grouped by index now that each index owns a
	// texture, and a second copy of this grouping in the renderer is the fourth
	// copy of a partition `PartitionSurfaces` exists to have prevented.
	//
	// The run must already hold only instances that show a surface;
	// `PartitionSurfaces` is what produces one.
	//
	// @param instances The draw list the order refers to.
	// @param order     The mirror run, sorted in place.
	// @param base      Where that run starts in the whole order, because what a
	//                  pass submits is an offset into the instance buffer and
	//                  not into this span.
	// @param opaque    Whether this is the opaque run. Opaque runs are also
	//                  split by shadow casting and fill `OpaqueFirst`; blended
	//                  runs fill `BlendedFirst` and never reach the shadow pass.
	// @param runs      Filled for the indices that appear, left alone for the
	//                  ones that do not. An index at or above `MAX_SURFACES` is
	//                  dropped rather than written past the end.
	// @since v0.8
	void GroupSurfaces(
		std::span<const DrawInstance> instances,
		std::span<uint32_t> order,
		uint32_t base,
		bool opaque,
		SurfaceRun (&runs)[MAX_SURFACES]
	);

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
	//     [Reflected,         Opaque)            mirror, grouped by surface
	//     [Opaque,            Opaque + Transparent)  blended, far to near
	//
	// The mirror run is subdivided by `Runs`, one entry per surface index, and
	// the casters sit at the front of each of those rather than at the front of
	// the mirror run as a whole.
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
		// `Transparency` went above zero - which is where the mirror flag is set
		// - so the reflection did not dim, it disappeared, and the pane fell back
		// to its own tint. That reads as the surface camera having stopped rather
		// than as an ordering rule.
		//
		// They sort back-to-front among themselves and are drawn after every
		// other blended instance, which is the "always draws on top" the feature
		// asks for. **Across the two runs the depth order is therefore not
		// strictly back-to-front**: a blended pane in front of a mirror is drawn
		// before it. Stated rather than hidden, and the trade is deliberate -
		// one sorted run per flag is what lets the mirror flag be a uniform
		// instead of a per-fragment branch on data the shader does not have.
		uint32_t TransparentSurfaces = 0;

		// Shadow casters among `Surfaces`, summed over every surface.
		//
		// **A total now, and no longer a range.** It was "the casters, contiguous
		// from `Reflected`" while every mirror shared one texture; grouping the
		// mirror run by surface index took that contiguity away, because one run
		// cannot be both grouped by index and split by caster. The per-index
		// ranges are in `Runs` and the shadow pass walks them. This survives as
		// the count, which is what a statistic and a test want.
		uint32_t SurfaceCasters = 0;

		// Where each surface's instances are, indexed by surface number.
		//
		// **Indexed rather than packed, so a lookup is not a search.** Both
		// passes that draw mirrors already know which index they are drawing -
		// the surface pass because it is excluding its own, the screen pass
		// because it walks the views it was given - and a packed list would make
		// every one of those a linear scan for a number that is already an
		// array subscript.
		SurfaceRun Runs[MAX_SURFACES];
	};

	// Folds one more value into a signature.
	//
	// **Exported so there is one mixing function rather than two.** The renderer
	// has to add its own terms - a surface camera's projection matrix and the
	// opacity it composites with, neither of which is a `shared` idea - and a
	// second mixer written beside this one would make the combined number depend
	// on which file computed which half.
	//
	// @param hash The signature so far. Any value; there is no reserved one.
	// @param word What to fold in.
	// @return The new signature.
	// @since v0.8
	uint64_t MixSignature(uint64_t hash, uint64_t word);

	// What a draw list looks like, as one number.
	//
	// **The question this answers is "would drawing this again produce the same
	// image", and it is asked because nothing cheaper can be.** A draw list is
	// written through `ecs::Store::EachBatchParallel`, which sets no dirty bit by
	// design, so there is no record of what moved - the same hole
	// `replication::ChangeDetection::Signature` exists to close for components,
	// and this is that idea applied to a render target.
	//
	// `render::Renderer` uses it to skip a surface pass that would redraw the
	// texture its slot already holds: a room of mirrors costs a pass per mirror
	// on the frames something moves and none on the frames nothing does.
	//
	// **Field by field, never over the object's bytes**, and this is the one
	// choice here worth defending because today it buys nothing. `DrawInstance`
	// is packed as it stands - `core::Name` is a four-byte id, `Color3` ends
	// four-aligned, nothing pads - so a byte-wise hash would agree with this one
	// on every list anybody can currently build.
	//
	// It is a property of the current field order and not a guarantee. One
	// `double`, one pointer, or one reordering opens an interior hole, and a
	// byte hash would then be folding in whatever the draw list's allocation
	// last held. The consequence is not a crash: the signature simply never
	// matches, every surface renders every frame, and the skip quietly stops
	// working with nothing to notice. `Reserved` is the same argument already
	// made - it exists so the object representation is deterministic across a
	// process boundary, and it says nothing about what is drawn, so a signature
	// that depended on it would be depending on padding by name.
	//
	// **What an instance shows is deliberately not in it.** A pane's index,
	// placement, size and tint all change what another mirror sees of it and are
	// all here. Its rendered *image* also changes what another mirror sees of it,
	// and including that would make every surface dirty every other one every
	// frame - A's image moves, so B must redraw, which moves B's image, so A must
	// redraw - a cycle that never settles and skips nothing. The cost of leaving
	// it out is that in a scene where the only thing moving is a reflection, the
	// recursion freezes rather than propagating another bounce. Anything moving
	// in the world thaws it on the next frame.
	//
	// @param instances The draw list.
	// @return A signature. Equal signatures mean equal lists; unequal ones
	//         mean the lists differ, or collided, and a collision costs a
	//         skipped redraw rather than a wrong one.
	// @since v0.8
	uint64_t SignatureOf(std::span<const DrawInstance> instances);

	// Divides one view's draw list into the runs its passes submit.
	//
	// Orders the list - opaque in world order, blended back to front from `eye`
	// - then moves mirrors to the back of the opaque head, then moves shadow
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
