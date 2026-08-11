#pragma once

// Which panel shows a world, when somebody asks to see one.
//
// **The half of "View" that can be silently wrong, in a header for
// `studio/Presentation.hpp`'s reason**: `Editor` needs a window, a device and a
// universe to construct, so a decision made inside it is one no test can reach.
// This one has three ways to be wrong and none of them looks like a fault:
//
// - **Opening a second panel on a world already on screen.** `PresentWorld`
//   round-robins one panel per frame, so two views of one world halve the rate
//   of both. The picture is correct and half as smooth, which reads as the
//   editor being slow rather than as a panel too many.
// - **Taking a panel somebody has pointed at another scene.** That is the editor
//   rearranging a layout on its owner's behalf, and the scene they were watching
//   is simply gone.
// - **Minting a panel when a free one exists.** Every panel is a
//   `render::SceneTarget` and a turn in the rotation, kept for the session.
//
// @tier L13 · client

#include <engine/world/World.hpp>

#include <cstddef>
#include <span>

namespace studio {

	// No panel. Also what `ChooseViewportFor` says when one has to be made.
	//
	// **Not zero, because zero is the main viewport** — the one index in this
	// program that is a real panel and reads as "none" to anybody who has met a
	// null handle first.
	//
	// @since v0.14
	inline constexpr size_t NO_VIEWPORT = static_cast<size_t>(-1);

	// One extra viewport panel, as this decision sees it.
	//
	// @since v0.14
	struct PanelView {
		// The world it is pinned to, or an invalid id when it follows the
		// active scene.
		engine::world::WorldId World;

		// Whether the panel exists on screen.
		bool Open = false;
	};

	// Which panel should show a world.
	//
	// **A panel already showing it wins over everything**, including a free one:
	// somebody pressing View on a row they are already looking at is asking
	// where it is, not for a second copy.
	//
	// **The main panel is offered only when it would show this world anyway.** It
	// follows the active scene and cannot be pinned, so handing it back for some
	// other world would be a panel that shows the wrong thing the moment anybody
	// clicks a scene.
	//
	// @param world     The world wanted. An invalid id has no panel.
	// @param mainWorld What the main panel draws — the active scene.
	// @param mainOpen  Whether the main panel is on screen.
	// @param panels    The extras, in index order. Index `i` here is panel
	//                  `i + 1`, which is the numbering `Editor::ExtraAt` uses.
	// @return The panel index, or `NO_VIEWPORT` when a new one has to be made.
	// @since v0.14
	size_t ChooseViewportFor(
		engine::world::WorldId world,
		engine::world::WorldId mainWorld,
		bool mainOpen,
		std::span<const PanelView> panels
	);
}
