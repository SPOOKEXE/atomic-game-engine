#pragma once

// Events emitted by GUI input routing and delivered to script hosts.
//
// @tier L7 · shared

#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <string>

namespace engine::gui {

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

		// Secondary button equivalents of `InputBegan` and `InputEnded`.
		//
		// Kept separate because a 3D adornment can capture both buttons at once,
		// and a receiver must know which gesture ended without asking a device
		// whose state may already have advanced to the next frame.
		MouseButton2Began,
		MouseButton2Ended,

		// Motion while a captured adornment button remains down. Unlike
		// `MouseMoved`, each kind names the button that owns the capture.
		MouseButton1Changed,
		MouseButton2Changed,

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

		// The virtual source identity carried by the command that was hit. Empty
		// identity means this event came from an ordinary authored instance.
		//@{
		ecs::Entity Collection;
		std::string Key;
		uint32_t Index = UINT32_MAX;
		//@}
	};

}
