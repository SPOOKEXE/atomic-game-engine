#pragma once

// The services around the 2D tree, and the one behaviour they exist to give.
//
// `scene::InstallServices` furnishes the world with `Workspace`, `StarterGui`
// and the rest. It cannot furnish this one: `GuiService` is a `gui` class and
// `gui/AGENTS.md` refuses an edge in either direction between these two modules,
// so the 2D tree brings its own installer and a host calls both.
//
// ## Why there is one service here and not three
//
// The plan named `GuiService`, `Path2D` and `GuidRegistryService`. Only the
// first is here, and the other two are refused for the reason the version's own
// rule gives — the one that kept `VideoFrame` out of the class tree:
//
// > Fifty registered classes with no layout, no rendering and no input would
// > put `TextLabel` in the insert palette, let somebody parent one under a
// > `Part`, save it into a game file — and then draw nothing, forever, with no
// > error. That is worse than not having it.
//
//   - **`Path2D`** draws a stroked polyline, and `gui::DrawKind` has four
//     members: `Rectangle`, `Outline`, `Image` and `Text`. There is no command
//     a path could compile to and no painter that could draw one, so the class
//     would be a set of control points nothing ever looked at. It arrives with
//     a fifth `DrawKind` and `ui::PaintGui`'s support for it, together.
//   - **`GuidRegistryService`** hands out registry entries for Roblox's own
//     internal bookkeeping. There is nothing here for it to keep, and a service
//     whose every method returns nothing is a shim wearing a familiar name.
//
// `GuiService` is here because it has something to do: it owns the selection,
// and `GuiObject::Selectable` has been a declared property with no reader since
// the tree was registered.
//
// ## Selection is the behaviour
//
// **Directional, over the compiled draw list, not over the tree.** The list is
// already flattened, clipped and in paint order — the same reason `Pick` walks
// it backwards rather than descending — and a second traversal that re-derived
// which elements are on screen would be a second answer to that question.
//
// @tier L7 · shared

#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/gui/DrawList.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// Which way a selection move goes.
	//
	// @since v0.8
	enum class SelectionMove : uint8_t {
		Up,
		Down,
		Left,
		Right,
	};

	// Adds this module's services to a world.
	//
	// **Idempotent, and separate from `scene::InstallServices` because the two
	// modules may not link each other.** A host calls both; calling either
	// twice does nothing the second time, which is what lets the studio run
	// them after every load without checking which kind of file it got.
	//
	// @param store The world to furnish.
	// @return The `GuiService` instance.
	ecs::Entity InstallGuiServices(ecs::Store &store);

	// The world's `GuiService`, or a null entity when it has none.
	//
	// @param store The world.
	// @return The service instance.
	ecs::Entity GuiServiceOf(const ecs::Store &store);

	// The strip at each edge a `ScreenGui` keeps clear unless it says not to.
	//
	// Roblox's `GuiService:GetGuiInset()` answers two corners; this answers the
	// top-left one, because nothing in this engine reserves anything at the
	// bottom and returning a second zero would imply somebody had decided it.
	//
	// **Read from the `Screen` the caller laid out against**, rather than
	// stored: the inset is a property of the surface being drawn to, and a
	// service holding its own copy would be a second answer that drifts the
	// first time a panel resizes.
	//
	// @param screen The screen the world was laid out against.
	// @return The reserved offset from the top-left, in pixels.
	core::Vector2 GuiInset(const Screen &screen);

	// Points the selection at one element, or clears it.
	//
	// **Refuses an element that cannot be selected**, rather than accepting it
	// and leaving the selection somewhere a move can never leave — which is the
	// state a game recovers from by rebooting. An element is selectable when it
	// carries `Element::Selectable`.
	//
	// @param store    The world.
	// @param instance The element to select, or `ecs::NULL_ENTITY` to clear.
	// @return Whether the selection changed.
	bool Select(ecs::Store &store, ecs::Entity instance);

	// Moves the selection one step in a direction.
	//
	// **Nearest along the axis, breaking ties across it**, which is what a
	// player means by "the one above this". Scored rather than sorted: the
	// candidate set is one frame's visible elements, and a comparator would
	// have to be a total order over a relation that is not one — B can be above
	// A while A is above C.
	//
	// Candidates are the `Selectable` elements of `list`, which is this frame's
	// compiled draw list — so an element scrolled out of view or under a
	// disabled collector is not reachable, because it is not in the list.
	//
	// **With nothing selected it seeds rather than moves**, picking the
	// first selectable element in paint order — unless
	// `GuiServiceState::AutoSelectGuiEnabled` is false, which is a game saying
	// it drives selection itself.
	//
	// @param store The world.
	// @param list  This frame's compiled list.
	// @param move  Which way to go.
	// @return Whether the selection changed.
	bool SelectNext(ecs::Store &store, const DrawList &list, SelectionMove move);
}
