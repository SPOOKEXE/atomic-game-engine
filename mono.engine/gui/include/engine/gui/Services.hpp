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

// `Screen`, which `GuiInset` takes, and `STARTER_GUI`/`PLAYER_GUI`, which
// `ResetPlayerGui` reads.
//
// **Added when this header stopped being included after `Layout.hpp` by
// accident.** Every caller happened to include that one first, so the missing
// include was invisible until `mono.server` reached for `ResetPlayerGui` on its
// own — a header is only self-contained when something proves it.
#include <engine/gui/Layout.hpp>

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
	// **There is deliberately no script binding for this, and the reason is the
	// argument above.** A script reaches `GuiService` through
	// `game:GetService("GuiService")` and its three settings are ordinary
	// declared properties, so what a game *decides* is reachable; what it cannot
	// reach is this, because the answer is a fact about the surface being drawn
	// and the scripting layer at L9 cannot name a `Screen`. It would also be
	// `(0, 0)` in every world today — `Screen::TopInset` is zero because this
	// engine has no top bar — so a binding would be a member that exists, answers
	// one constant forever and looks decided. `PlayerGui`'s `SetTopbarTransparency`
	// pair is absent for the same missing thing; `script/src/GuiMethods.cpp` names
	// it where an author would look for it.
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

	// Rebuilds a player's interface from the world's `StarterGui`.
	//
	// **Roblox's respawn rule, and this engine did not have it at all.**
	// `Layout` draws a `ScreenGui` from `StarterGui` *or* from a player's
	// `PlayerGui` — see `STARTER_GUI` — which is a shortcut that works in single
	// player and is wrong the moment there are two of them: every client draws
	// the same instances, so a script that hid one player's health bar hid
	// everybody's, and a script that parented something into `StarterGui` at
	// runtime gave it to everyone at once. `StarterGui` is a *template*. What a
	// player sees is their own copy.
	//
	// **`ResetOnSpawn` is what decides whether a copy survives a death**, and it
	// is the field that has been on `Layer` since v0.8 with nothing reading it.
	// The rule, which is Roblox's:
	//
	//   1. Every collector already in the player's `PlayerGui` whose
	//      `ResetOnSpawn` is true is destroyed. That is the default, and it is
	//      what makes a fresh life start with a fresh interface.
	//   2. Every collector whose `ResetOnSpawn` is false stays exactly as it is,
	//      with whatever state a script has put in it. That is what the field is
	//      *for* — a minimap or a settings panel that must not blink on death.
	//   3. Every child of `StarterGui` is then cloned in, **unless a child of
	//      that name survived step 2**. A survivor is the copy the player
	//      already has; cloning beside it would give them two, one of which
	//      nothing ever updates.
	//
	// **Cloned rather than moved**, so the template is still there for the next
	// player and the next life. Anything under `StarterGui` that is not a
	// collector is copied too — a `Folder` of assets, a `ModuleScript` — because
	// what Roblox copies is the container's contents rather than a filtered set,
	// and a rule that filtered would silently drop the one thing a game put
	// there.
	//
	// **Called by whoever spawns**, not by a system here. `scene::LoadCharacter`
	// cannot call it — `scene` may not link `gui`, which is the refusal
	// `gui/AGENTS.md` states and `STARTER_GUI` above is the other half of — so
	// the host that decides a player has respawned is the one that says so. That
	// is the same split `replication::Authority::SetInterest` uses for the rule
	// about who may see what.
	//
	// @param store  The world.
	// @param player The `Player` whose `PlayerGui` to rebuild. Anything without
	//        one is left alone and answers zero, which is a player some host
	//        built without going through `scene::AddPlayer`.
	// @return How many children were cloned in. Zero for a world with no
	//         `StarterGui`, an empty one, or a player whose surviving copies
	//         already cover it — none of which is a failure.
	// @since v0.15
	size_t ResetPlayerGui(ecs::Store &store, ecs::Entity player);
}
