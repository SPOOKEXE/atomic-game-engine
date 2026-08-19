#pragma once

// Which element a pointer is over, and what that means happened.
//
// **Hit testing is a backwards walk of the compiled draw list, not a tree
// walk.** The list is already in paint order, already clipped and already
// sorted by `ZIndex` - which is precisely the front-to-back order a hit test
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
// An element takes input when it is `Active`, or when it is a `GuiButton` -
// Roblox's rule. A plain `Frame` is decoration and the click goes through it to
// whatever is behind, which is the behaviour that lets a background panel exist
// without swallowing the interface it contains.
//
// @tier L7 · shared

#include <engine/core/types/UDim.hpp>
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
	// The alternative - a call per platform event - would make the order two
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

		// Restricts the hit test to one collector after a world-space pointer
		// has been projected onto its canvas. Null leaves the list unrestricted.
		ecs::Entity Collector;

		// Ignores spatial collectors while testing ordinary window pixels.
		bool ScreenOnly = false;

		// How far the wheel turned this frame, in notches.
		//
		// **Positive is a turn away from the person, which moves the canvas
		// *back* towards its start.** That is SDL's sign and Dear ImGui's, and
		// it is the one a person's hand expects - pushing the wheel away pushes
		// the page up. A router that added it instead would scroll every list
		// the wrong way, which is the kind of thing nobody writes a test for
		// until it has shipped.
		//
		// **Notches rather than pixels**, because how many pixels a notch is
		// worth is this module's decision and not the window's: the same wheel
		// event has to move a list by the same amount whether it arrived through
		// SDL, through the editor's viewport panel or through a test.
		//
		// @since v0.18
		float Wheel = 0.0f;
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

		// A press landed on a `TextBox` and the keyboard went to it.
		//
		// **The pair below is the only thing in this enum that is not about the
		// pointer**, and it is here because the pointer is what decides it: a
		// press is the one gesture that says "type into this one and not that
		// one". `gui::Focus` is where the fact comes to rest.
		//
		// @since v0.15
		Focused,

		// A press landed elsewhere and the keyboard left the box it was in.
		//
		// **Not emitted for a focused box that was destroyed.** The event names
		// an element and a dead element has nothing to fire at, which is
		// `Router::Forget`'s argument: firing at something that is no longer
		// there is worse than firing nothing. `FocusedTextBox` answers null the
		// frame after either way, so nothing reads as still focused.
		//
		// @since v0.15
		FocusReleased,

		// A press landed on an element a `UIDragDetector` is attached to.
		//
		// **The three below name the *detector* and not the element**, which is
		// Roblox's arrangement and the one that lets two detectors on one panel
		// mean two different gestures. `GuiEvent::Position` is where the pointer
		// is and `GuiEvent::Local` is how far it has moved since the press,
		// which is the number a `Scriptable` drag is for.
		//
		// @since v0.18
		DragBegan,

		// The pointer moved while a drag was held.
		DragContinue,

		// The pointer was released and the drag ended.
		DragEnded,
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

		// Whether Return is what ended it. Only read on `FocusReleased`.
		//
		// **The one field here the router never sets**, and that is what it is
		// for: a press releasing a box is `Router::Update`'s to report and a
		// Return releasing one is `gui::Type`'s, and both owe a script the same
		// `FocusLost` with Roblox's `enterPressed` telling them apart. A caller
		// that submits a box builds the event itself - `TypeResult::Released`
		// says when - because no press happened and no element was picked.
		//
		// @since v0.15
		bool Entered = false;
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

	ecs::Entity PickInCollector(
		const ecs::Store &store, const DrawList &list, ecs::Entity collector, const core::Vector2 &point
	);

	ecs::Entity PickScreen(const ecs::Store &store, const DrawList &list, const core::Vector2 &point);

	// Every element under a point within one subtree, front to back.
	//
	// **`Pick`'s question without the `Active` filter and scoped to a
	// container**, which is what `PlayerGui:GetGuiObjectsAtPosition` asks: a
	// decorative `Frame` is transparent to *input* and is still an object that
	// is under the pointer, and a player asking what is under theirs must not be
	// told about somebody else's interface.
	//
	// **Ordered by `Resolved::Order`, which is the compile's own answer read
	// back rather than derived again.** `gui/AGENTS.md` refuses a second
	// traversal that re-decides what is on top, and this is why there does not
	// have to be one: `Compiled::Rebuild` writes each element's paint position
	// into its `Resolved`, so sorting by it descending *is* front to back. A
	// world nothing has compiled has every `Order` at zero, and `Resolved::Depth`
	// - which the layout writes - breaks the tie the way paint order would, with
	// the deeper element in front.
	//
	// @param store The world.
	// @param root  The container to search under, itself excluded. A `PlayerGui`
	//        at the call site this exists for.
	// @param point The position, in canvas pixels.
	// @param out   Filled in, front to back. Cleared first.
	// @return How many were found.
	// @since v0.18
	size_t ElementsAt(
		const ecs::Store &store, ecs::Entity root, const core::Vector2 &point, std::vector<ecs::Entity> &out
	);

	// Turns a polled pointer into events, and remembers enough to do it.
	//
	// Long-lived, one per canvas being driven. It holds the hover and the press
	// across frames - which is the whole of its state, and none of it belongs
	// in the store: rule 2 is about data another module also reads, and nobody
	// replicates where a mouse is.
	//
	// **The keyboard focus is the exception, and it is why `Update` takes a
	// mutable store.** Two modules read which `TextBox` is focused - this one, to
	// know whether a press changed it, and the scripting layer, for
	// `UserInputService:GetFocusedTextBox` - and L9 has no route to a router. So
	// the fact lives in `GuiServiceState::FocusedTextBox` and this class decides
	// it, which is rule 2 applied rather than avoided. Where a mouse is has one
	// reader and stays here.
	//
	// @since v0.8
	class Router {
	  public:
		// Works out what changed since the last call.
		//
		// **Mutable, because a press moves the keyboard focus** and that fact is
		// the world's rather than this object's - see the note above. Nothing
		// else here writes to the store, and a world with no `GuiService` is
		// routed exactly as it was before focus existed.
		//
		// @param store   The world.
		// @param list    The compiled list for this frame.
		// @param pointer Where the pointer is and whether it is down.
		// @return The events, in the order they happened. Valid until the next
		//         call - the vector is reused rather than reallocated, because
		//         this runs every frame and produces nothing most of them.
		std::span<const GuiEvent> Update(ecs::Store &store, const DrawList &list, const Pointer &pointer);

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
		// For a caller whose canvas went away - a panel closed, a world
		// unloaded - where firing a `MouseLeave` at an element that no longer
		// exists would be worse than firing nothing.
		//
		// **The keyboard focus is deliberately not forgotten here**, and it
		// could not be: it belongs to the world rather than to this object, and
		// a router being torn down is not somebody having finished typing. A
		// caller that means to release it calls `gui::Focus` with a null entity.
		void Forget() {
			Over = ecs::NULL_ENTITY;
			Holding = ecs::NULL_ENTITY;
			Dragging = ecs::NULL_ENTITY;
			Detector = ecs::NULL_ENTITY;
			Dragged = ecs::NULL_ENTITY;
		}

		// The `ScrollingFrame` whose bar is being dragged, or null.
		//
		// **A grabbed bar is not a pressed element and does not become one.** It
		// emits no `InputBegan` and no `Activated`, because nothing in the tree
		// was pressed - a scroll bar is chrome the frame draws rather than an
		// instance a script can connect to. Exposed so a host can tell "the
		// pointer is busy" from "the pointer is over nothing".
		//
		// @since v0.18
		ecs::Entity ScrollBarHeld() const {
			return Dragging;
		}

		// The `UIDragDetector` a gesture is running through, or null.
		//
		// @since v0.18
		ecs::Entity DragHeld() const {
			return Detector;
		}

	  private:
		// Starts a drag if the press landed on something with a detector.
		//
		// @return Whether one began.
		bool BeginDrag(ecs::Store &store, const DrawList &list, const core::Vector2 &point);

		// Moves the dragged element to follow the pointer.
		void ContinueDrag(ecs::Store &store, const core::Vector2 &point);

		// Applies a wheel turn to whatever under the pointer can take it.
		//
		// @return The frame that scrolled, or null.
		ecs::Entity Wheel(ecs::Store &store, const core::Vector2 &point, float notches);

		// Moves a held bar's canvas to follow the pointer.
		void DragBar(ecs::Store &store, const core::Vector2 &point);

		std::vector<GuiEvent> Events;
		ecs::Entity Over;
		ecs::Entity Holding;

		// --- the scroll bar drag ---------------------------------------------
		//
		// **Held here for the reason the hover and the press are**: where a
		// pointer is part-way through a gesture is this object's state and
		// nobody replicates it. What the drag *produces* is
		// `Scrolling::CanvasPosition`, which is authored and does cross.
		ecs::Entity Dragging;
		bool DragVertical = false;

		// Where inside the thumb the pointer took hold, along the dragged axis.
		// Without it a bar jumps so its top-left corner is under the cursor the
		// moment it is grabbed.
		float DragGrab = 0.0f;

		// --- the element drag ------------------------------------------------
		//
		// **Held here rather than on the component**, which is the same rule the
		// scroll bar above follows: where a gesture is part-way through is this
		// object's, and what it *produces* - `Element::Position` - is the world's.
		// A `UIDragDetector` carrying its own "being dragged" flag would be one
		// two clients writing one replicated row.
		//@{
		ecs::Entity Detector;
		ecs::Entity Dragged;
		core::Vector2 DragFrom;
		core::UDim2 DragStart;
		float DragAngle = 0.0f;
		//@}

		core::Vector2 Last;
		bool WasDown = false;
		bool Started = false;
	};
}
