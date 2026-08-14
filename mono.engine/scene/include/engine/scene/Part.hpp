#pragma once

// `Part` - a class, not a component.
//
// A class in the `ecs::Classes` sense: a name, a parent, and the component set
// its instances land in. `Instance.new("Part")` from script in v0.6 and
// `MakePart` from C++ today are the same operation through the same table, so
// there is exactly one answer to "what is a part made of" and the editor, the
// loader, the bindings and the tests cannot disagree about it.
//
// The tree is Roblox's, and shallow on purpose - `ecs/Classes.hpp` names these
// four as the shape it stores ancestry for:
//
//     Instance                 nothing; the root everything derives from
//     └─ PVInstance            Transform - anything with a place in the world
//        └─ BasePart           Bounds, Visual, Collider, Surface
//           └─ Part            the concrete box
//
// **Inheritance is set inclusion**, so a query for `Transform` matches every
// part without knowing parts exist, and `:IsA("BasePart")` is an ancestry scan
// rather than a second table.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Everything that decides what one part is.
	//
	// A description rather than a builder, so making a part is one value and
	// one call. `Size` is a full extent because that is what a person types and
	// what Roblox calls it; `Bounds` and `Collider` store the half, and
	// `MakePart` is the one place that halving happens.
	//
	// @since v0.4
	struct PartDesc {
		// Where it goes, in world space.
		core::CFrame Frame;

		// Full extent on each local axis, in metres.
		core::Vector3 Size{1.0f, 1.0f, 1.0f};

		// What it feels like, naming a row in the world's `SurfaceTable`.
		core::Name Material;

		// What it looks like, naming a mesh a presentation module resolves.
		core::Name Mesh;

		// Which `BasePart` class to mint, or an invalid id for a plain `Part`.
		//
		// **Here so that `MakePart` stays the only constructor.** A
		// `SpawnLocation` is a `Part` with one component more, and a caller that
		// assembled it by hand would be the second definition
		// `scene/AGENTS.md` refuses - the two disagree the first time
		// `BasePart` gains a component. Anything that does not derive from
		// `BasePart` is refused rather than half-built.
		//
		// @since v0.15
		ecs::ClassId Class;

		// What it collides as.
		ShapeKind Shape = ShapeKind::Box;

		// Whether the world may move it.
		//
		// **This decides whether `RigidBody` and `Motion` exist on the entity
		// at all**, rather than setting a flag the physics step reads. An
		// anchored part therefore lands in a different archetype, and the
		// dynamic queries never visit it - which is the ECS-native form of the
		// optimisation and strictly better than a branch per row per tick.
		bool Anchored = false;
	};

	// The `Part` class id.
	//
	// Registers the class tree on the first call, so a caller cannot use the id
	// before the table has it. Later calls are a load.
	//
	// @return The registered class id for `"Part"`.
	// Where an instance's handle is, in world space.
	//
	// **Roblox's `PVInstance:GetPivot()`.** A `Transform` says where the centre
	// of something is; a pivot says where it is *held*, which is what an author
	// places against - a door by its hinge, a lid by its rim. `Pivot::Offset`
	// carries why that needs storage rather than being derivable.
	//
	// @param store    The world.
	// @param instance The instance.
	// @return The world pivot, or the identity for something with no placement.
	// @since v0.10
	core::CFrame PivotOf(const ecs::Store &store, ecs::Entity instance);

	// Moves an instance so its handle lands on `target`.
	//
	// **The inverse of `PivotOf`, and that is the whole of it**: the placement
	// that puts the pivot at the target is `target * Offset⁻¹`. Doing it the
	// other way round - setting the transform to the target and hoping - is what
	// "PivotTo does not respect the offset" bugs are.
	//
	// @param store    The world.
	// @param instance The instance.
	// @param target   Where the handle should end up.
	// @return `false` for something with no placement to move.
	// @since v0.10
	bool PivotTo(ecs::Store &store, ecs::Entity instance, const core::CFrame &target);

	// Registers the whole scene class tree, once per process.
	//
	// **Every class accessor here calls this first**, so a caller asking for
	// `Humanoid` or `Attachment` still gets a fully registered tree - one
	// registration whichever door it is entered by.
	//
	// **Named rather than spelled `PartClass()`, which is what it used to be.**
	// The call is made for its side effect and not for the id it returns, and
	// asking for the *Part* class in order to look up a *Humanoid* reads as a
	// claim about the hierarchy - which it is not. A humanoid derives from the
	// instance root, not from a part. What every accessor shares is the
	// registration, so that is what this is called.
	//
	// Nothing else registers these names: `ecs::Classes::RegisterInstanceRoot`
	// declares `Instance` itself and stops there, so reaching the tree through
	// it would look up a name nobody had registered and cache the miss.
	// The volume a collider encloses, in cubic metres.
	//
	// **One rule, because two would drift.** A mass derived one way by the
	// solver and shown another way by a properties panel is a part that weighs
	// two different amounts depending on who is asking.
	//
	// @param collider The shape and its half-extents.
	// @return The volume, or zero for a shape with no extent.
	// @since v0.14
	float VolumeOf(const Collider &collider);

	// What a part weighs, in kilograms.
	//
	// **Density wins where it is set, and `RigidBody::Mass` is what is written
	// down otherwise.** A part with `PhysicsProperties::Custom` weighs its
	// density times its volume - so resizing it changes what it weighs, which is
	// what density means - and a part without one weighs whatever was authored.
	//
	// **Derived at the point of use rather than written back.** A system that
	// wrote `Mass` every tick would be a second copy of a fact, dirtying a
	// replicated component to say a number that had not changed.
	//
	// @param collider   The shape it collides as.
	// @param body       Its rigid body.
	// @param properties Its overrides, or null for a part that has none.
	// @return The mass the solver should use. Never negative.
	// @since v0.14
	float MassOf(const Collider &collider, const RigidBody &body, const PhysicsProperties *properties);

	// Registers the whole scene class tree, once per process.
	//
	// **The one caller of the registration, which is what makes every accessor
	// beside it safe to cache.** `PartClass` and its siblings hold a static id
	// each; those statics call this one rather than racing a registration of
	// their own, and a class id is fixed for the life of the process once it
	// exists.
	//
	// Idempotent, and cheap after the first call. Anything that names a class
	// before a store has been furnished - a loader, a test - calls this first.
	void EnsureClassTree();

	// The `Part` class id, registering the whole tree on first call.
	//
	// @return The class id.
	ecs::ClassId PartClass();

	// The `Camera` class id, registering the tree on first call.
	//
	// **A camera is an instance, because a camera is a row.** `scene::Camera`
	// has been a component since v0.4 so a world can hold several; what was
	// missing was a class, so `Instance.new("Camera")` had nothing to resolve
	// to. Derives from `PVInstance`: it has a place in the world and is neither
	// drawn nor collided.
	//
	// @return The class id.
	ecs::ClassId CameraClass();

	// The `Sound` class id, registering the tree on first call.
	//
	// **Derives from `Instance` and not from `PVInstance`**, because a sound
	// has no place of its own - where it is heard from is its parent's. Under
	// `Workspace` it is heard everywhere at one level; inside a part it is
	// heard from that part and falls off with distance. That is Roblox's rule
	// and it is also the one that keeps "attach a sound to a thing" as
	// `Parent = thing` rather than a second field naming what the hierarchy
	// already says.
	//
	// @return The class id.
	ecs::ClassId SoundClass();

	// Creates one part in a world.
	//
	// The single place that decides what a part is made of. Anything building
	// parts another way - a loader, a test, a demo - is a second definition,
	// and the two disagree the first time one of them gains a component.
	//
	// The instance starts from the class prototype and is then written from
	// `desc`, so a component added to the class but not named in `PartDesc`
	// keeps its declared default instead of being silently zeroed.
	//
	// `PartDesc::Class` is what keeps that true for the classes that are a part
	// plus something - `SpawnLocation` is the first - rather than growing a
	// second builder beside this one.
	//
	// @param store The world to create in.
	// @param desc  What the part is.
	// @return The new entity, or `ecs::NULL_ENTITY` when the store refused to
	//         create one - an adopt-only replica does - or when
	//         `PartDesc::Class` does not derive from `BasePart`.
	ecs::Entity MakePart(ecs::Store &store, const PartDesc &desc);
}
