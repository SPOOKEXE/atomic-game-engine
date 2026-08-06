#pragma once

// The meshes the engine ships with, as geometry rather than as a special case.
//
// **A built-in is an ordinary `MeshData` that nobody had to publish**, and that
// is the whole design. `scene::ShapeKind` already names a box, a sphere and a
// cylinder for the *collider*; until this file the renderer could draw exactly
// one of them, so a sphere collided as a sphere and drew as a cube. Making the
// built-ins produce the same type an importer produces means the renderer holds
// one path — a name resolves to a mesh — instead of a table of shapes plus a
// table of meshes that would drift the first time one of them gained a feature.
//
// **Generated rather than tabulated.** A hand-written sphere is four hundred
// vertices somebody has to review, and reviewing it means checking winding by
// eye. The generators below are twenty lines each and the *winding* is what the
// suite checks, over every built-in at once — which is the property that
// actually matters and the one a table makes unreadable.
//
// **Everything is a unit shape about its own origin**, spanning -0.5 to +0.5 on
// each axis. `render::Renderer` folds `DrawInstance::HalfExtent` into the model
// matrix, so a unit mesh is what turns one sphere into every ball in the scene.
// A generator that returned a radius-one sphere would make every part twice the
// size it says it is, and the mistake would look like a physics bug because the
// collider would still be right.
//
// @tier L8 · shared

#include <engine/assets/Mesh.hpp>

#include <cstdint>
#include <string_view>

namespace engine::assets {

	// Which built-in.
	//
	// **A closed list, and it is not on any wire** — unlike `AssetKind`, whose
	// ordinals cross a manifest. What crosses here is the *name*, because
	// `AGENTS.md` rule 4 says a mesh reference has to survive a save file and a
	// rename, so `Visual::Mesh` holds `engine.Sphere` and never a number. The
	// enum is the in-process form and may be reordered freely.
	//
	// @since v0.9
	enum class BuiltinMesh : uint8_t {
		// The unit cube. What an unnamed mesh has always drawn as.
		Cube,

		// A single quad on the XZ plane facing +Y. The cheapest thing a mirror
		// can be, and the reason it is here.
		Plane,

		// A box whose top edge sits at -Z and whose slope runs down to the +Z
		// bottom edge, so the sloped face looks towards +Z and up. Roblox's
		// `WedgePart`.
		Wedge,

		// A wedge sloping on two axes at once, meeting at one raised corner.
		// Roblox's `CornerWedgePart`.
		CornerWedge,

		// A UV sphere of radius 0.5. Smooth normals, because a sphere drawn
		// with flat ones reads as a disco ball rather than as a sphere.
		Sphere,

		// A cylinder of radius 0.5 about the **Y** axis, half a metre each way.
		//
		// Y and not X, and the axis is worth stating because Roblox's cylinder
		// lies along X. This one matches `scene::Collider::Extent`, whose own
		// documentation reads "cylinder radius in X and half-height in Y" — a
		// mesh whose axis disagreed with its collider would fall over on a
		// slope it visually rested on.
		Cylinder,
	};

	// How many built-ins there are.
	//
	// @since v0.9
	constexpr uint8_t BUILTIN_MESH_COUNT = 6;

	// The stable name a `Visual::Mesh` holds for a built-in.
	//
	// **Namespaced under `engine.`**, so a game publishing content called
	// `Sphere` cannot take over the built-in one. Content resolution is by
	// name, and a flat namespace makes that a race between whoever registers
	// first.
	//
	// @param mesh Which built-in.
	// @return A view valid for the lifetime of the process.
	std::string_view BuiltinName(BuiltinMesh mesh);

	// Parses what `BuiltinName` wrote.
	//
	// @param name The name to look up.
	// @param out  The built-in, written only on a match.
	// @return `false` when `name` is not a built-in — which is the ordinary
	//         answer for every published mesh and is not an error.
	bool BuiltinFromName(std::string_view name, BuiltinMesh &out);

	// Builds one.
	//
	// Deterministic: the same built-in produces byte-identical geometry every
	// call and in every process, which is what lets a client and a publisher
	// agree about `engine.Sphere` without either of them shipping it.
	//
	// @param mesh Which built-in.
	// @return The geometry, with bounds already computed. Always valid.
	MeshData MakeBuiltin(BuiltinMesh mesh);
}
