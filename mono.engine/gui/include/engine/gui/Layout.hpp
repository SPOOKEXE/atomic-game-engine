#pragma once

// Turning what an author wrote into where it actually is.
//
// One pass, parent before child, writing `Resolved` per node. Everything after
// this - the draw list, the hit test, a script asking `AbsoluteSize` - reads
// that component with a query and nothing walks the tree a second time.
//
// **Why the tree is walked here and nowhere else.** A `UDim2` means nothing
// without a parent rectangle, so resolving one is inherently top-down; that is
// the single place the shape of the tree is unavoidable. Every consumer
// downstream wants a flat list in paint order, which is what `Compile.hpp`
// produces once and keeps.
//
// ## What is deliberately approximate, stated rather than discovered
//
// **Text is measured with a constant advance.** `AVERAGE_ADVANCE` below is the
// fraction of an em a glyph is assumed to occupy, and `TextScaled` shrinks the
// size until the string fits by that estimate. The exact answer needs a glyph
// atlas, which is a `client` thing this module may not have - and the important
// property is not exactness but that **there is one answer**: a backend that
// re-measured with real metrics would disagree with the hit test and with what
// a headless test asserts. So the backend draws at `Resolved::TextSize` and
// does not second-guess it.
//
// **`AutomaticSize` measures children, and a labelled element measures its
// string.** The second half used to be a refusal, on the grounds that growing a
// box to an estimate produces a box the text spills out of. It does not, and the
// reason is the paragraph above: nothing downstream re-measures, so within this
// engine `AVERAGE_ADVANCE` is not an approximation of the truth - it *is* the
// measurement, the one answer the hit test, a headless assertion and the
// renderer all agree on. A box grown to it fits by the same definition of
// fitting the module uses everywhere else, and `TextScaled` on a grown axis
// recovers exactly the size it started from because `FittedTextSize` divides by
// the product the growth multiplies.
//
// What is still true is that the estimate may be wrong about real glyphs. That
// risk is not introduced by growing - it is the risk `TextScaled` has carried
// since v0.8 - and closing it means metrics shared *below* L7 rather than a
// second opinion at the point of use.
//
// **The failure the old refusal also named is still refused, and now by
// construction rather than by a branch.** A `TextLabel` has no children, so an
// implementation that measured children and did not notice the text would
// collapse every labelled element an author set the property on to nothing at
// all. A labelled element is sized from its text, so that is not the path it
// takes.
//
// @tier L7 · shared

#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Entity.hpp>

#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// The screen a `ScreenGui` collects onto.
	//
	// Passed in rather than read from a resource, because the two callers have
	// different answers and both are right: a game fills the window, and the
	// studio's viewport panel fills a rectangle inside it. A module that read
	// "the window size" from somewhere global could not serve the second.
	//
	// **`Screen` and not `Viewport`**, because `gui::Viewport` is already the
	// component behind `ViewportFrame` - a 3D view drawn *inside* the tree,
	// which is very nearly the opposite of this.
	//
	// @since v0.8
	struct Screen {
		// How wide the canvas is, in pixels.
		float Width = 1600.0f;

		// How tall, in pixels.
		float Height = 900.0f;

		// The strip at the top a `ScreenGui` keeps clear unless it says not to.
		//
		// Roblox's is 36 pixels of top bar. Zero here by default because this
		// engine has no top bar of its own yet, and a reserved strip nothing
		// occupies is a band of dead space an author cannot explain.
		float TopInset = 0.0f;
	};

	// The fraction of an em an average glyph is assumed to advance.
	//
	// One number, in one place, used by the size fit and by nothing else. See
	// the note at the top of this file for why an estimate is the right shape
	// here and why the backend must use the result rather than its own.
	constexpr float AVERAGE_ADVANCE = 0.52f;

	// How much taller than its em size a line of text is drawn.
	constexpr float LINE_SPACING = 1.2f;

	// The containers a `LayerCollector` may draw from, by name.
	//
	// **Roblox's containment rule, and it is a rule rather than a style
	// choice.** A `ScreenGui` parented to a `Part` draws nothing - not because
	// it is invisible but because nothing is looking at that part of the tree -
	// and an engine that drew it anyway would let an author ship a game whose
	// interface appears in the studio and not in the client, which is the worst
	// direction for a difference like that to run.
	//
	//   - a `ScreenGui` draws from `STARTER_GUI` or from a player's
	//     `PLAYER_GUI`. The studio shows the first; a client shows the second.
	//   - a `SurfaceGui` or a `BillboardGui` draws from those *and* from
	//     `WORKSPACE`, because each is attached to something in the world and
	//     the world is where that something lives.
	//
	// **These are `scene`'s service names, spelled again here**, because
	// `gui/AGENTS.md` refuses an edge to `scene` - the same refusal that made
	// `SurfaceGui::Face` re-declare `NormalId`'s six members. They are exposed
	// rather than kept in the source file so a test can pin them against the
	// service table they are copied from, which is the arrangement that turns a
	// rename into a failing test instead of an interface that quietly stops
	// drawing.
	//
	// @since v0.8
	//@{
	inline constexpr std::string_view WORKSPACE = "Workspace";
	inline constexpr std::string_view STARTER_GUI = "StarterGui";
	inline constexpr std::string_view PLAYER_GUI = "PlayerGui";
	//@}

	// Resolves every `LayerCollector` in the store and everything under it.
	//
	// Writes `Resolved` on each node reached and clears `Resolved::Rendered` on
	// each node that is not - a disabled collector, an invisible ancestor, an
	// element parented outside any collector. Nothing is destroyed and nothing
	// is zeroed: an element scrolled out of view keeps the rectangle it had, so
	// the hit test does not have to tell "off screen" from "never laid out".
	//
	// @param store    The world.
	// @param screen The screen a `ScreenGui` collects onto.
	// @return How many nodes were reached and marked rendered.
	size_t Layout(ecs::Store &store, const Screen &screen);
}
