#pragma once

// `Part` — a class, not a component.
//
// A class in the `ecs::Classes` sense: a name, a parent, and the component set
// its instances land in. `Instance.new("Part")` from script in v0.6 and
// `MakePart` from C++ today are the same operation through the same table, so
// there is exactly one answer to "what is a part made of" and the editor, the
// loader, the bindings and the tests cannot disagree about it.
//
// The tree is Roblox's, and shallow on purpose — `ecs/Classes.hpp` names these
// four as the shape it stores ancestry for:
//
//     Instance                 nothing; the root everything derives from
//     └─ PVInstance            Transform — anything with a place in the world
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

		// What it collides as.
		ShapeKind Shape = ShapeKind::Box;

		// Whether the world may move it.
		//
		// **This decides whether `RigidBody` and `Motion` exist on the entity
		// at all**, rather than setting a flag the physics step reads. An
		// anchored part therefore lands in a different archetype, and the
		// dynamic queries never visit it — which is the ECS-native form of the
		// optimisation and strictly better than a branch per row per tick.
		bool Anchored = false;
	};

	// The `Part` class id.
	//
	// Registers the class tree on the first call, so a caller cannot use the id
	// before the table has it. Later calls are a load.
	//
	// @return The registered class id for `"Part"`.
	ecs::ClassId PartClass();

	// Creates one part in a world.
	//
	// The single place that decides what a part is made of. Anything building
	// parts another way — a loader, a test, a demo — is a second definition,
	// and the two disagree the first time one of them gains a component.
	//
	// The instance starts from the class prototype and is then written from
	// `desc`, so a component added to the class but not named in `PartDesc`
	// keeps its declared default instead of being silently zeroed.
	//
	// @param store The world to create in.
	// @param desc  What the part is.
	// @return The new entity, or `ecs::NULL_ENTITY` when the store refused to
	//         create one — an adopt-only replica does.
	ecs::Entity MakePart(ecs::Store &store, const PartDesc &desc);
}
