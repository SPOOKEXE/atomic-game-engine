#pragma once

// Whether the editor may write its viewport camera's lens this frame.
//
// **A viewport panel's camera is an instance, so an author can select it and
// edit it - and until this existed, three of its properties could not be
// edited at all.** `EnsureViewerCamera` assigned the whole `scene::Camera`
// component every frame from a lens the editor derives, so typing 35 into
// `FieldOfView` held for exactly one frame and snapped back to 69.9008 degrees:
// the default 1.22 radians, in degrees, which is the fingerprint that says the
// editor wrote it rather than a script. The same held for `NearPlane` and
// `FarPlane`, and it held in Edit, Run and Play alike.
//
// The comment two lines above that assignment already stated the rule for the
// *placement* - "writing the eye into the camera every frame would fight an
// author dragging its CFrame in the properties panel - the view would snap back
// on the next frame and the field would look broken" - and the lens beneath it
// did exactly that.
//
// **The editor cannot simply stop writing**, which is why this is a decision
// rather than a deletion. The far plane is derived from the fly speed: flying
// fast across a large world with a 500 metre far plane clips the world away, and
// an author who has never touched the field should not have to find it. So the
// editor keeps the lens until somebody takes it, and then leaves it alone.
//
// **In a header for `studio/Presentation.hpp`'s reason.** `Editor` needs a
// window, a device and a universe to construct, so a decision made inside it is
// one no test can reach - and a decision a test cannot reach is a decision that
// gets to be wrong for a release. This one was.
//
// @tier L13 · client

#include <engine/scene/Components.hpp>

#include <optional>

namespace studio {

	// What the editor should write into a viewport camera's lens, if anything.
	//
	// **Ownership is decided by comparison rather than by a flag**, and the
	// comparison is against what this panel last wrote rather than against a
	// default. An author who sets `FieldOfView` to exactly the editor's own
	// value has changed nothing, and a panel whose fly speed has moved the far
	// plane has not been taken over by anybody.
	//
	// **Once taken, it stays taken**, and that falls out rather than being
	// arranged: a caller that only records `written` when it actually writes
	// leaves `written` disagreeing with the instance for as long as the author's
	// value stands, so every later frame takes the same branch. There is no
	// second state to keep in step.
	//
	// Re-minting the camera - a snapshot restore, a panel repointed at another
	// world - is expressed by passing no `written`, which hands the lens back to
	// the editor. That is right: the row an author edited is gone.
	//
	// @param onInstance What the `Camera` component holds right now.
	// @param written    What this panel last wrote, or nothing if it has not
	//                   written one - which is the case on the frame the camera
	//                   is minted.
	// @param derived    What the editor would like the lens to be: its default
	//                   with the far plane stretched to the fly speed.
	// @return The lens to assign, or nothing to leave the instance alone. A
	//         caller that writes must record the same value as `written` for the
	//         next frame.
	// @since v0.15
	std::optional<engine::scene::Camera> ViewerLensToWrite(
		const engine::scene::Camera &onInstance,
		const std::optional<engine::scene::Camera> &written,
		const engine::scene::Camera &derived
	);
}
