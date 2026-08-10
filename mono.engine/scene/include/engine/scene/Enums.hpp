#pragma once

// The named kinds this module's components come in.
//
// Four closed sets, and closed is the point. A shape is not an open extension
// point: every one of them costs an exact narrow-phase pair against every other
// shape, so adding a case here is a decision about how much collision code
// exists rather than a value somebody drops in. A box has six faces and will go
// on having six.
//
// **They live here rather than beside the component that holds them**, which is
// where `NormalId` was written first: an enum next to one struct reads as that
// struct's private business, and the moment a second thing needs a face — a
// decal, a surface gui, a hinge — it is either included through a component
// header that has nothing to do with it or copied. One file for the named sets
// is what stops the second of those.
//
// **Names are the format, numbers are not — with one deliberate exception.**
// `BodyKind` and `ShapeKind` sit in components whose registration declares a
// writer that writes them by name, so their underlying numbers stay free to move
// and must never be written anywhere by hand.
//
// `NormalId` is the exception and it is one on purpose: it is stored as its
// ordinal in a trivially-copied component, so the number *is* the format. That
// is why its values are written out below rather than left to the compiler, and
// why they are Roblox's — a `Face` of 1 has to mean `Top` in a game file this
// engine wrote and in one it did not. **Reordering that enum is a format
// change**, which is exactly the thing this paragraph exists to stop somebody
// doing casually to the other two.
//
// @tier L7 · shared

#include <engine/core/types/Vector3.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::scene {

	// How the solver is allowed to move a body.
	//
	// Separate from whether the entity has a `RigidBody` at all. A part with no
	// `RigidBody` is not a static body — it is not a body, and no query the
	// physics pipeline runs will ever visit it. This says what to do with the
	// ones it does visit.
	//
	// @since v0.4
	enum class BodyKind : uint8_t {
		// Never moved by the solver and never integrated. Its transform is
		// whatever put it there.
		Static,

		// Moved by whoever owns it — a platform, an animation, a script — and
		// never by a contact. Pushes dynamic bodies and is pushed by nothing.
		Kinematic,

		// Moved by forces and contacts. The ordinary case.
		Dynamic,
	};

	// What shape a collider actually is.
	//
	// Three, because the exact narrow phase needs a pair function per unordered
	// pair and three shapes is already six of them. A fourth is ten.
	//
	// @since v0.4
	enum class ShapeKind : uint8_t {
		// A box, half-extents on each local axis.
		Box,

		// A sphere, radius in X.
		Sphere,

		// A cylinder about the local Y axis, radius in X and half-height in Y.
		Cylinder,
	};

	// Returns a stable, human-readable name for a body kind.
	//
	// For logs and diagnostics. Not a format: nothing parses these back.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(BodyKind kind);

	// Returns a stable, human-readable name for a shape kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ShapeKind kind);

	// Which face of a box a thing points out of.
	//
	// **Roblox's `Enum.NormalId`, including its names.** `Top` and `Bottom`
	// rather than Up and Down, for the reason `Part.cpp` gives about the class
	// tree: a script written against Roblox says `Enum.NormalId.Top`, and a
	// second spelling of one face is the duplicate `scene/AGENTS.md` calls the
	// most expensive kind of debt.
	//
	// The values are Roblox's ordinals too, so a saved game file carrying a
	// number means the same thing in both places.
	//
	// @since v0.7
	enum class NormalId : uint8_t {
		Right = 0,
		Top = 1,
		Back = 2,
		Left = 3,
		Bottom = 4,
		Front = 5,
	};

	// The outward unit normal of a face, in the box's own space.
	//
	// **`Front` is -Z, which is the one entry worth checking rather than
	// assuming.** Roblox's front face looks down negative Z and so does this
	// engine's camera, so a mirror on the front of a pane faces the same way an
	// unrotated camera does. Getting it backwards puts the reflection behind the
	// pane, which renders the clear colour and reads as a broken mirror.
	//
	// @param face Which face.
	// @return The outward normal, in local space.
	constexpr core::Vector3 NormalOf(NormalId face) {
		switch (face) {
		case NormalId::Right:
			return core::Vector3{1.0f, 0.0f, 0.0f};
		case NormalId::Top:
			return core::Vector3{0.0f, 1.0f, 0.0f};
		case NormalId::Back:
			return core::Vector3{0.0f, 0.0f, 1.0f};
		case NormalId::Left:
			return core::Vector3{-1.0f, 0.0f, 0.0f};
		case NormalId::Bottom:
			return core::Vector3{0.0f, -1.0f, 0.0f};
		case NormalId::Front:
			return core::Vector3{0.0f, 0.0f, -1.0f};
		}
		return core::Vector3{0.0f, 0.0f, -1.0f};
	}

	// How a surface's alpha channel is treated.
	//
	// **A closed list whose ordinals are on the wire**, so an entry may be
	// appended and never reordered — `assets::TextureFormat`'s rule and for the
	// same reason.
	//
	// @since v0.9
	enum class AlphaMode : uint8_t {
		// The alpha channel is ignored and the surface is solid. What almost
		// every texture is, and the only mode that costs nothing.
		Opaque = 0,

		// A fragment below `SurfaceAppearance::AlphaCutoff` is discarded and
		// everything else is solid.
		//
		// **The mode a character model needs.** Hair, eyelashes and foliage are
		// authored as cut-out planes on a sheet with a mask; blending them puts
		// every plane in the sorted pass, where a hundred overlapping strands
		// cost a hundred sorted draws and still get the order wrong. A discard
		// keeps them opaque, keeps them in the depth buffer and keeps them out
		// of the sort.
		Clip = 1,

		// The alpha channel is a blend factor, so the surface joins the sorted
		// transparent pass.
		Blend = 2,
	};

	// Who may see what a service holds.
	//
	// **Replication is not implemented from this yet, and the field is here
	// anyway.** It is the fact that distinguishes `ServerStorage` from
	// `ReplicatedStorage`, and a pair of containers that differ only in their
	// name is a pair somebody will use interchangeably until the day one of
	// them starts leaking server state to clients.
	//
	// @since v0.7
	enum class ServiceScope : uint8_t {
		// Both halves see it. `ReplicatedStorage`, `Workspace`, `Lighting`.
		Shared,

		// The server only. `ServerScriptService`, `ServerStorage`.
		Server,

		// The client only. The `Starter*` services, which are templates a
		// client copies rather than content a server simulates.
		Client,
	};

	// What a surface camera's image is put through before it is shown.
	//
	// **A closed list rather than a shader name**, and that is a deliberate
	// limit rather than a stub. This pipeline has one fragment program for
	// opaque geometry; a mirror naming an arbitrary program would need a
	// pipeline per program, a compilation path, and an answer to what happens
	// when the file is missing on somebody else's machine — which is the render
	// graph `ROADMAP.md` puts behind a prototype project, not a field.
	//
	// What a closed list *does* buy is exactly what a mirror wants: a camera
	// feed, a scope, a scanner. Each is a grade over an image that is already
	// rendered, so each is a handful of lines in `opaque.frag` and costs nothing
	// for the surfaces that do not ask.
	//
	// **The ordinals are on the wire**, so an entry may be appended and never
	// reordered — `AlphaMode`'s rule, and for the same reason.
	//
	// @since v0.13
	enum class SurfaceEffect : uint8_t {
		// The reflection as rendered. What every mirror was before this existed.
		None,

		// An image intensifier: everything to green, lifted, grained and
		// vignetted.
		NightVision,

		// A heat map. Luminance stands in for temperature, which is a lie an
		// engine with no thermal model cannot avoid and should say out loud —
		// it looks right because bright things in a lit scene usually are the
		// hot ones.
		Thermal,

		// A security camera: grey, scanlined, and with a bright band rolling
		// down it.
		Cctv,

		// The image twisted about its own centre, turning slowly. The one entry
		// here that moves texels rather than grading them.
		Swirl,
	};

	// How many there are, for a settings list and for the shader's bound.
	inline constexpr size_t SURFACE_EFFECT_COUNT = 5;

	// Returns the effect's name.
	//
	// Round-trips like `Describe(NormalId)` below, and for the same reason: this
	// is the name `ecs::EnumTable` registers and a script assigns.
	//
	// @param effect The effect to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(SurfaceEffect effect);

	// Returns the face's name.
	//
	// **This one round-trips, unlike its two neighbours.** `Describe(BodyKind)`
	// is for a log line and nothing parses it back; these names are the ones
	// `ecs::EnumTable` registers and a script assigns, so they are the surface
	// rather than a diagnostic. Kept here beside the enum so the two cannot
	// drift — `scene/Part.cpp` registers the same six in the same order, and the
	// order is the format.
	//
	// @param face The face to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(NormalId face);
}
