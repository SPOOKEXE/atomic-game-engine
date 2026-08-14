#pragma once

// What `ContextActionService` has claimed, and the report a handler is handed.
//
// **The stack is shared and the callables are not**, which is `Signals.hpp`'s
// arrangement one door along and for the same reason. What a bound action *is* —
// a name, the keys it claims, a priority, and the rule that the highest claim
// hears the press first and decides whether anything below it does — names no
// VM, and the order two actions at one priority fire in is a thing a recording
// depends on. Two hand-written copies of that would agree until the first time
// one was fixed.
//
// **What a frame *did* is here too, and for the same argument one size down.**
// The four report builders and the pair behind `gameProcessedEvent` answer
// questions about an input rather than about the stack, and two pumps answering
// either one is two answers — an engine where a click carried a position, or
// read as handled, in one language and not the other is one nobody could port a
// handler between.
//
// A Luau handler is a registry ref and a JavaScript one an index into
// `JsContext::Callables`, so a callable crosses as an opaque `CallbackRef`
// exactly as a connection's does. Nothing here may interpret one.
//
// **The stack lives on a VM's context and never on the world.** A bound action
// holds a callable: it cannot cross a snapshot, cannot be replicated, and dies
// with the runtime that registered it. A world resource holding one would be a
// resource that cannot be serialised.
//
// @tier L9 · shared
// @since v0.16

#include "Signals.hpp"

#include <engine/core/Name.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/gui/Input.hpp>
#include <engine/scene/Input.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {

	// One input event, as a script sees it.
	//
	// **Roblox's `InputObject`, and what this engine used to hand nobody.** Both
	// languages build a value of their own from one of these — a tagged userdata
	// on the Luau side and an object of a registered class on the JavaScript one
	// — which is the same split `ScriptCall::ReturnSignal` is on: the *fact* is
	// one thing and only its wrapper is two.
	//
	// Trivially copyable on purpose: the Luau half placement-news it into
	// userdata memory, exactly as every value type in `LuauValues.cpp` is.
	struct InputReport {
		// Where the pointer was, in pixels from the top-left of the window,
		// with the wheel's notches in Z.
		//
		// **Roblox's placement exactly**, including the odd-looking Z: a wheel
		// notch has nowhere else to go in a `Vector3`, and a script migrated
		// from a Roblox place reads `input.Position.Z` for it.
		core::Vector3 Position;

		// How far it moved since the previous frame, in the same space.
		core::Vector3 Delta;

		// `Begin`, `Change` or `End`, as an `Enum.UserInputState` member.
		//
		// **A name rather than an ordinal**, because the member list is
		// registered in `scene/Part.cpp` and an ordinal here would be a second
		// statement of its order — the kind that agrees until somebody inserts a
		// member.
		core::Name State;

		// The key, or `Unknown` for anything that is not one. Roblox reports
		// `Enum.KeyCode.Unknown` for a mouse event and so does this.
		scene::KeyCode Key = scene::KeyCode::Unknown;

		// Where it came from.
		scene::InputSource Source = scene::InputSource::Keyboard;
	};

	// --- the four reports a frame can produce ---------------------------------
	//
	// **All four are shared, since `UserInputService` stopped being Luau's.**
	// The key report always was — both pumps hand one to a bound action — and the
	// other three lived beside the Luau signal loop while that loop was the only
	// one. Two pumps building a report each is two answers to "what did this
	// frame do", which is the drift the whole neutral layer exists to close: an
	// engine where a click carried a position in one language and not in the
	// other would be one nobody could port a handler between.

	// A report for one key edge.
	//
	// Position and delta are zero, as Roblox's are: a keyboard event has no
	// place on the screen.
	//
	// @param key   Which key moved.
	// @param began Whether it went down.
	// @return The report a handler is given.
	InputReport KeyReport(scene::KeyCode key, bool began);

	// A report for one mouse button edge, or for one held button.
	//
	// **The delta is left at zero even though the pointer may have moved this
	// frame**, which is Roblox's shape: motion is reported by its own
	// `InputChanged`, and putting it on the click as well would have a handler
	// that sums deltas count the same movement twice.
	//
	// @param input  This frame's input.
	// @param button Which button moved.
	// @param began  Whether it went down.
	InputReport ButtonReport(const scene::InputState &input, scene::MouseButton button, bool began);

	// A report for this frame's pointer motion.
	InputReport MotionReport(const scene::InputState &input);

	// A report for this frame's wheel movement.
	InputReport WheelReport(const scene::InputState &input);

	// Whether the 2D interface has the pointer this beat.
	//
	// **What `gameProcessedEvent` actually is here, and it is shared for the
	// reason the four builders above are**: two pumps deciding what "the game
	// already handled this" means is two answers to one question, and a place
	// ported between the languages would find its click swallowed in one and not
	// the other.
	//
	// **The router's own events are the evidence, because they are the only
	// record of a press being taken.** `gui::Router` emits an event naming an
	// element exactly when the pointer is over or pressed on something that takes
	// input — `Pick` walks past anything inactive — so an event in this beat's
	// queue *is* the interface having consumed the pointer. `MouseLeave` is the
	// one exception and it is the important one: it is emitted for the element
	// the pointer has just left, which is the frame the interface stopped having
	// it.
	//
	// **A keyboard press is never processed, and that is a gap rather than a
	// decision.** Roblox's answer is true while a `TextBox` has focus, and `gui`
	// has no focus at all — `Router` tracks a hover and a press and nothing else,
	// and a `TextBox` is a class that draws. Closing it needs the router to hold a
	// focused element and release it on a press elsewhere, which is a change in
	// `gui` rather than here; until then a key is honestly unprocessed, which is
	// what it is. `DEFERRED.md` D00117 carries the whole of what it would take.
	//
	// @param events What the router produced for this beat, in its order.
	// @return `true` when a pointer event this beat should read as handled.
	bool InterfaceHasPointer(std::span<const gui::GuiEvent> events);

	// Whether an input report describes the pointer rather than the keyboard.
	//
	// The filter `InterfaceHasPointer` is applied through: the interface takes
	// clicks, moves and wheel notches, and nothing it does can consume a key.
	//
	// @param report The report.
	// @return `true` for a button, a motion or a wheel notch.
	bool IsPointerReport(const InputReport &report);

	// One action bound through `ContextActionService`.
	//
	// @since v0.10
	struct BoundAction {
		// What the script called it. Unbinding is by this name.
		std::string Name;

		// The keys it claims, as `scene::KeyCode` ordinals.
		//
		// **Ordinals rather than the enum**, which costs one cast at each end
		// and keeps the stack's rules readable without the input vocabulary
		// beside them.
		std::vector<uint16_t> Keys;

		// The VM's name for the handler.
		CallbackRef Callback = 0;

		// Higher wins. Roblox's default is zero and so is this.
		int Priority = 0;
	};

	// One bound action as a script sees it, which is Roblox's
	// `GetBoundActionInfo` record.
	//
	// **`InputReport`'s shape one door along, and it is here for that struct's
	// reason.** The record holds `Enum.KeyCode` members, and `ScriptValue` — the
	// tree a method may return — has no tag for an `EnumItem` and must not gain
	// one, because it crosses a world and `Codec.hpp` is a wire format. So the
	// *fact* is one struct and only the wrapper is two: each VM builds the table
	// or object and its own `EnumItem`s from this, exactly as each builds its own
	// `InputObject`.
	//
	// **The keys arrive as member names rather than as ordinals**, so
	// `scene::Describe` is called once here rather than once per adapter — the
	// same rule `KeyReport` and its three neighbours keep.
	//
	// @since v0.16
	struct BoundActionReport {
		// What the script bound it as, and the key it is reported under.
		std::string_view Name;

		// The keys it claims, as `Enum.KeyCode` member names.
		std::span<const core::Name> Keys;

		// Higher wins. Roblox calls this `priorityLevel`.
		int Priority = 0;

		// **Inverted from this engine's own order, so higher still wins.**
		// `ActionStack` is sorted highest priority *first* and Roblox's
		// `stackOrder` counts the other way — the largest number is the claim
		// that gets the key. Reporting the index under Roblox's name would be the
		// same word meaning the opposite thing, which is worse than not reporting
		// it.
		int StackOrder = 0;

		// Accepted and ignored at bind time, for the reason `BindAction` gives,
		// so it is always false rather than whatever was passed.
		bool CreateTouchButton = false;
	};

	// Every action one VM has bound, highest priority first.
	//
	// @since v0.16
	class ActionStack {
	  public:
		// Binds an action, replacing one of the same name.
		//
		// **Rebinding a name replaces it rather than stacking a second.**
		// Roblox's behaviour, and the one that makes a script safe to run twice:
		// a reload that bound the same action again would otherwise fire its
		// handler twice per press, forever.
		//
		// **Sorted here rather than searched at press time**, because binding is
		// rare where pressing is not — and stably, so two actions at one
		// priority fire in bind order. That is the only tie-break that is
		// reproducible; an unstable sort would make which one wins depend on the
		// allocator.
		//
		// @param action   What to bind. Moved from.
		// @param released Filled in with the callable a replaced action held.
		// @return `true` when something was replaced and `released` is set.
		bool Bind(BoundAction action, CallbackRef &released);

		// Drops one action by name.
		//
		// @param name     What it was bound as.
		// @param released Filled in with the callable it held.
		// @return `false` when nothing of that name is bound.
		bool Unbind(std::string_view name, CallbackRef &released);

		// Drops every action.
		//
		// @param released Appended with every callable the stack held.
		void UnbindAll(std::vector<CallbackRef> &released);

		// The next action claiming a key, at or after a position in the stack.
		//
		// **A walk rather than a single answer, because
		// `Enum.ContextActionResult.Pass` exists.** The highest claim gets the key
		// first and *decides* whether anything below it hears about it: returning
		// `Sink`, or returning nothing, stops the press here, and returning `Pass`
		// hands it down. Answering only the winner made `Pass` unimplementable and
		// made a handler's return value dead.
		//
		// **An index and not an iterator, so a handler may rebind mid-walk without
		// a dangling pointer.** A handler that binds or unbinds shifts the stack
		// under this loop; the position is then approximate, which is the honest
		// cost of letting a handler do it at all — Roblox does not define that case
		// either. What it must not be is a use-after-free.
		//
		// @param key      A `scene::KeyCode` ordinal.
		// @param position Where to start. Advanced past whatever is returned.
		// @return The action, or null when nothing else claims it.
		const BoundAction *ClaimingFrom(uint16_t key, size_t &position) const;

		// One action by name, or null.
		const BoundAction *Find(std::string_view name) const;

		// Every action, highest priority first.
		std::span<const BoundAction> Entries() const;

	  private:
		std::vector<BoundAction> Bound;
	};
}
