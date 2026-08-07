#pragma once

// Which element a pointer is over, and what that means happened.
//
// **Hit testing is a backwards walk of the compiled draw list, not a tree
// walk.** The list is already in paint order, already clipped and already
// sorted by `ZIndex` — which is precisely the front-to-back order a hit test
// wants, read from the end. A second traversal that re-derived that order would
// be a second answer to "what is on top", and the two would disagree the first
// time somebody changed a sort in one of them.
//
// ## Events out, signals elsewhere
//
// This module is L7 and `script::Signals` is L9, so nothing here can fire a
// `.Activated`. What it produces is a list of *events*, and whoever owns the
// scripting layer turns each into a signal. That is not a workaround: an editor
// driving a UI with no scripts running wants the same hit testing and none of
// the dispatch, and a test wants to assert on the events rather than on what a
// Luau handler did with them.
//
// ## What `Active` means, and why a button does not need it
//
// An element takes input when it is `Active`, or when it is a `GuiButton` —
// Roblox's rule. A plain `Frame` is decoration and the click goes through it to
// whatever is behind, which is the behaviour that lets a background panel exist
// without swallowing the interface it contains.
//
// @tier L7 · shared

#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/gui/DrawList.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// Where the pointer is and whether it is down.
	//
	// **A state, not a stream of deltas.** A caller polls its device once a
	// frame and hands over what it found; the router works out what changed.
	// The alternative — a call per platform event — would make the order two
	// events arrived in part of this module's contract, and SDL does not
	// promise one.
	//
	// @since v0.8
	struct Pointer {
		// Where it is, in canvas pixels.
		core::Vector2 Position;

		// Whether the primary button is down this frame.
		bool Down = false;

		// Whether the pointer is over the canvas at all.
		//
		// False when it has left the window, which is different from being over
		// nothing: leaving has to end a hover, and a position outside the
		// canvas would do that by accident rather than on purpose.
		bool Inside = true;
	};

	// What happened, in Roblox's vocabulary.
	//
	// @since v0.8
	enum class EventKind : uint8_t {
		// The pointer entered an element's rectangle.
		MouseEnter,

		// It left.
		MouseLeave,

		// It moved while over an element.
		MouseMoved,

		// The button went down over an element.
		InputBegan,

		// The button came up. Fired on the element the press *began* on, which
		// is what makes a drag off a button and back a single interaction.
		InputEnded,

		// The button went down and came up on the same element.
		//
		// **A separate event rather than a flag on `InputEnded`**, because it
		// is the one every script actually connects to and because the two
		// genuinely differ: releasing off the button ends the input and
		// activates nothing.
		Activated,
	};

	// One thing that happened to one element.
	//
	// @since v0.8
	struct GuiEvent {
		// What happened.
		EventKind Kind = EventKind::MouseMoved;

		// The element it happened to.
		ecs::Entity Instance;

		// Where the pointer was, in canvas pixels.
		core::Vector2 Position;

		// Where it was relative to the element's top-left corner. What a drag
		// handle wants, and it is here because the router already knows the
		// rectangle and the caller would have to look it up again.
		core::Vector2 Local;
	};

	// The element under a point, or null.
	//
	// Walks `list` from the end, which is front to back, and returns the first
	// element that takes input and whose rectangle and clip both contain the
	// point.
	//
	// @param store The world, for the `Active` test and the class test.
	// @param list  The compiled list, in paint order.
	// @param point The pointer, in canvas pixels.
	// @return The element, or `NULL_ENTITY`.
	ecs::Entity Pick(const ecs::Store &store, const DrawList &list, const core::Vector2 &point);

	// Turns a polled pointer into events, and remembers enough to do it.
	//
	// Long-lived, one per canvas being driven. It holds the hover and the press
	// across frames — which is the whole of its state, and none of it belongs
	// in the store: rule 2 is about data another module also reads, and nobody
	// replicates where a mouse is.
	//
	// @since v0.8
	class Router {
	  public:
		// Works out what changed since the last call.
		//
		// @param store   The world.
		// @param list    The compiled list for this frame.
		// @param pointer Where the pointer is and whether it is down.
		// @return The events, in the order they happened. Valid until the next
		//         call — the vector is reused rather than reallocated, because
		//         this runs every frame and produces nothing most of them.
		std::span<const GuiEvent>
		Update(const ecs::Store &store, const DrawList &list, const Pointer &pointer);

		// The element the pointer is over, or null.
		//
		// Feed this back into `CompileRequest::Hovered` so an
		// `AutoButtonColor` button lights up. That is a deliberate one-frame
		// loop: the hover is computed from the list the *previous* compile
		// produced, so a button that appears under a stationary pointer lights
		// up on the frame after it appears. One frame, and the alternative is a
		// compile that depends on its own output.
		ecs::Entity Hovered() const {
			return Over;
		}

		// The element the button went down on, or null.
		ecs::Entity Pressed() const {
			return Holding;
		}

		// Forgets the hover and the press without emitting anything.
		//
		// For a caller whose canvas went away — a panel closed, a world
		// unloaded — where firing a `MouseLeave` at an element that no longer
		// exists would be worse than firing nothing.
		void Forget() {
			Over = ecs::NULL_ENTITY;
			Holding = ecs::NULL_ENTITY;
		}

	  private:
		std::vector<GuiEvent> Events;
		ecs::Entity Over;
		ecs::Entity Holding;
		core::Vector2 Last;
		bool WasDown = false;
		bool Started = false;
	};
}
