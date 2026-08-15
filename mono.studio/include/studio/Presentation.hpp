#pragma once

// How far between two ticks the editor draws a world.
//
// **One line of arithmetic, in a header, because the version of it that lived
// inside `Editor::PresentWorld` was wrong for the editor's most ordinary
// state.** `Editor` needs a window, a device and a universe to construct, so
// nothing in it is reachable from a test - and a decision a test cannot reach
// is a decision that gets to be wrong for a release. `studio/Projection.hpp`
// makes the same argument about the viewport's arithmetic and for the same
// reason.
//
// ## What went wrong
//
// A world's `PreviousTransform` is written by `capture-previous`, a
// `PreSimulation` system. `World::Present` runs `PreRender` alone, so a world
// that is not being ticked never updates it - and the draw list interpolates
// *from* it. Present such a world at alpha zero and every part is drawn at
// whatever frame it was created with, which for a part the editor made is the
// identity: the origin.
//
// The editor knew this and asked the wrong question. It presented at alpha one
// when `Universe::StateOf` was not `Active` - but `Editor::SyncWorldStates`
// deliberately leaves *every* world `Active` when nothing is running, so that
// an author returning to Edit does not find their scenes marked stopped. Plain
// Edit mode is therefore: every world `Active`, `Editor::Simulate` returning
// before `Universe::Tick`, and an accumulator that never advances - alpha
// zero, and every part drawn at the origin while its selection outline, which
// reads `Transform` directly, followed the mouse.
//
// Two halves of the frame disagreeing about where something is reads as a
// renderer fault. It is not one, and this is the arithmetic that decides it.
//
// @tier L13 · client

#include <engine/world/Enums.hpp>

namespace studio {

	// Which alpha to present a world at.
	//
	// **Both arguments are needed and neither implies the other.** A world's
	// state says whether the driver *would* advance it; `advancing` says
	// whether the driver is being run at all. A suspended world in a ticking
	// universe and an active world in a host that has stopped ticking are both
	// standing still, and both have to be drawn at one.
	//
	// @param advancing   Whether the caller is ticking the universe this frame.
	//                    `Editor::Simulate` returns early in two cases - nothing
	//                    is running, and everything running is paused - and both
	//                    are this being `false`.
	// @param state       What the universe says about this world.
	// @param accumulator Where the world's clock is between two ticks, from
	//                    `Universe::AlphaOf`. Used only when the world is really
	//                    being advanced; it is stale otherwise, which is the
	//                    whole bug.
	// @return `accumulator` for a world that is being ticked, and `1.0f` - draw
	//         the current transform, there is nothing to interpolate towards -
	//         for one that is not.
	// @since v0.11
	float PresentationAlpha(bool advancing, engine::world::WorldState state, float accumulator);
}
