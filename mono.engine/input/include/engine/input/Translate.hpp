#pragma once

// SDL events into `scene::InputState`, which is the only place SDL keycodes are
// turned into names.
//
// **Here rather than in the client, because `Actions.hpp` already says the
// rule**: nothing outside this module names a key. `Actions` is the half that
// maps a key to an *intent* - F5 toggles the frame graph - and this is the half
// that reports what is held, which is what a game's own scripts need. Two halves
// of one question, and both belong to whichever module owns the keyboard.
//
// **It writes a `shared` type and reads a `client` one**, which is the whole
// point: `scene::InputState` is a resource a script can reach and an SDL event is
// not, and this is the one function that crosses between them.
//
// This module still pumps nothing. The caller owns the event loop - SDL insists
// on it on some platforms - and hands events here one at a time, exactly as it
// hands them to `Actions`.
//
// @tier L12 · client

#include <engine/scene/Input.hpp>

#include <SDL3/SDL_events.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace engine::input {

	// The platform-owned text that is visible while an input method has not
	// committed it yet. Positions are UTF-8 character offsets as SDL reports
	// them, and the revision lets a consumer distinguish a retained preedit from
	// a new platform update.
	struct TextComposition {
		static constexpr size_t MAXIMUM_BYTES = 4096; // Maximum UTF-8 preedit bytes.

		std::string_view Text; // Borrowed UTF-8 preedit text.
		int32_t Start = -1;	   // Selected preedit offset.
		int32_t Length = -1;   // Selected preedit length.
		uint64_t Revision = 0; // Platform update revision.
	};

	// One primary pointer sample in window pixels. Held motion samples let a
	// drag move before its release when all events arrive between two draws.
	struct PrimaryPointerSample {
		// Where the event occurred, in window pixels.
		core::Vector2 Position;
		// Whether the primary button was held at this sample.
		bool Down = false;
		// Ends capture without activating its target.
		bool Cancelled = false;
		// Held motion, which may replace the preceding held motion sample.
		bool Motion = false;
	};

	// Device-neutral edits the GUI applies in the order they arrived.
	enum class PresentationEditKind : uint8_t {
		// Committed UTF-8 text.
		Text,
		// Remove one grapheme or the selection.
		Backspace,
		// Submit a single-line entry or insert a line break.
		Submit,
		// Move the caret one grapheme left.
		CaretLeft,
		// Move the caret one grapheme right.
		CaretRight,
	};

	// A committed text fragment or one edit key, in SDL event order. Extend is
	// captured when a caret key arrives, before Shift may be released.
	struct PresentationEditEvent {
		// The edit to apply.
		PresentationEditKind Kind = PresentationEditKind::Text;
		// UTF-8 bytes for Text, empty for key edits.
		std::string Text;
		// Shift was down when a caret key arrived.
		bool Extend = false;
	};

	// Input owed to the next presentation. Simulation still reads the ordinary
	// per-update InputState; a draw can be skipped while events keep arriving.
	struct PresentationInput {
		// Holds the largest text event accepted by client control automation.
		static constexpr size_t MAXIMUM_TEXT_BYTES = 16384;
		// Upper bound on ordered primary pointer samples.
		static constexpr size_t MAXIMUM_PRIMARY_SAMPLES = 1024;
		// Upper bound on ordered text and edit key events.
		static constexpr size_t MAXIMUM_EDIT_EVENTS = 1024;
		// Upper bound on ordered key presses for semantic GUI actions.
		static constexpr size_t MAXIMUM_KEY_PRESSES = 1024;

		// Ordered key presses for GUI navigation and activation.
		std::vector<scene::KeyCode> KeyPresses;
		// Controller button presses since the last presentation, per slot.
		std::array<uint32_t, scene::MAX_CONTROLLERS> PressedControllerButtons{};
		// Ordered committed text and editing keys.
		std::vector<PresentationEditEvent> Edits;
		// Total retained UTF-8 bytes in Edits.
		size_t TextBytes = 0;
		// Vertical wheel notches owed to the next drawn pointer location.
		float WheelDelta = 0.0f;
		// Ordered primary pointer edges and held motion.
		std::vector<PrimaryPointerSample> PrimarySamples;
		// Samples were dropped and capture must be cancelled.
		bool PointerOverflowed = false;
		// Edits were dropped when either edit limit was reached.
		bool EditsOverflowed = false;
		// Key presses were dropped when their limit was reached.
		bool KeysOverflowed = false;
	};

	// Accumulates SDL events into one frame's input state.
	//
	// **A class rather than a free function, because a frame has a shape**: the
	// deltas accumulate across however many events arrive and then have to be
	// cleared, and the previous frame's key bits have to be kept to produce the
	// edges. A free function taking one event could do neither.
	//
	// @since v0.10
	// @client
	class Translator {
	  public:
		// Starts a frame: rolls the four `Previous` fields and clears the deltas.
		//
		// **The typed text is cleared and not rolled**, which is the difference
		// this class is arranged around: a key that is down stays down and its
		// edge is the difference between two frames, where a character was
		// produced once and has no previous value to be compared against.
		//
		// **Call once before pumping that frame's events, not after them** -
		// `Actions::BeginFrame`'s rule and for the same reason: a delta cleared
		// after the pump is a delta nobody ever read.
		//
		// The four are the keys, the buttons, the focus flag and the last source,
		// and every one of them is rolled here because every one of them is read
		// as an *edge* - `InputBegan`, `WindowFocused` and `LastInputTypeChanged`
		// are all the difference between two frames.
		void BeginFrame();

		// Folds one event into the state.
		//
		// Ignores anything that is not a key, typed text, a button, a motion, a
		// wheel or a focus change. **Returns whether it was consumed rather than
		// swallowing it**, because the caller also feeds `Actions` and an editor's interface
		// layer, and each of them decides for itself.
		//
		// Anything it does consume also stamps `InputState::LastSource`, which is
		// what `UserInputService:GetLastInputType` answers with. A focus change
		// deliberately does not: losing a window is not a device speaking, and a
		// place that swapped its prompts on an alt-tab would be reporting a
		// keyboard nobody touched.
		//
		// @param event The event from the caller-owned pump.
		// @return `true` when this event changed the state.
		bool HandleEvent(const SDL_Event &event);

		// The state as of the events handled so far.
		//
		// @return The state, to be copied onto the world's resource.
		const scene::InputState &State() const {
			return Current;
		}

		// Connected gamepads and raw joysticks, normalized into stable slots.
		const scene::ControllerState &Controllers() const {
			return ControllerCurrent;
		}

		// A host that routes a GUI opts in. Other translators retain no
		// presentation data and need no matching consume call.
		void EnablePresentationInput();

		// Edges and deltas retained across updates until a host routes one GUI
		// presentation. Pointer edges stay ordered so two clicks remain two clicks.
		const PresentationInput &PendingPresentation() const {
			return PresentationPending;
		}

		// A copy of controller levels with unconsumed button press edges included.
		// Controllers() keeps its ordinary update and tick semantics.
		scene::ControllerState ControllersForPresentation() const;

		// The physical primary level, except while a dropped gesture must wait
		// for release before it can start a new GUI press.
		bool PrimaryDownForPresentation() const;

		// Called once after the host has routed a presentation's input.
		void ConsumePresentation();

		// What this frame's keystrokes spelled, as UTF-8.
		//
		// **A different question from which keys are down, which is why it is a
		// string and not derivable from `State`.** A keycode is a position on a
		// keyboard; this is what the layout, the modifiers and any composition
		// the platform ran made of it, so `Shift` plus `1` is `!` here and two
		// key bits there. Empty on almost every frame, and empty is the answer
		// for a frame where somebody held a key down without producing a
		// character.
		//
		// **UTF-8, so one byte is not one character.** An accented letter is two
		// bytes and an emoji is four; anything that indexes this by byte will
		// cut one in half. `gui::Focus` counts characters the way this has to be
		// counted.
		//
		// `InputState` is trivially copyable, so this string stays in the input
		// adapter. A host that presents less often than it updates uses the
		// ordered `PendingPresentation().Edits` instead.
		//
		// @return This frame's text. Valid until the next `BeginFrame`.
		std::string_view TypedText() const {
			return Typed;
		}

		// The input method's current uncommitted text.
		//
		// **A level rather than a frame delta.** SDL only sends an editing event
		// when the candidate changes, so clearing this in `BeginFrame` would make
		// preedit disappear between platform events. A committed text event,
		// an empty editing event, or losing window focus clears it instead.
		TextComposition Composition() const {
			return TextComposition{Preedit, PreeditStart, PreeditLength, PreeditRevision};
		}

		// Clears every key and button, and this frame's typed text with them.
		//
		// **What losing focus does.** Alt-tabbing away while holding W must not
		// leave a character walking forever - SDL sends no key-up for a key
		// released in another window, so the release has to be manufactured here.
		// `scene::InputState::Focused` records why, and the character controller
		// checks it as a second belt.
		//
		// The text goes because it is a delta rather than a level, exactly as
		// `MouseDelta` is: a frame that ended with the window going away did not
		// finish delivering what was typed into it.
		void ReleaseAll();

	  private:
		// arch-waiver ecs-copy: this is where the frame's input is *built*, before
		// any world has one. `Translate.hpp`'s whole job is turning SDL events into
		// this shape; the store's copy is written from here and is the authority
		// from then on.
		scene::InputState Current;

		// SDL instance ids stay in this adapter. Worlds only see stable Gamepad1
		// through Gamepad8 slots, so a platform number never crosses the boundary.
		// arch-waiver ecs-copy: like `Current` above, this is the frame builder before
		// the value is copied into each world. The world resource is authoritative
		// after `Client::WriteInput` or Studio Play writes it.
		scene::ControllerState ControllerCurrent;
		PresentationInput PresentationPending;
		bool PresentationEnabled = false;
		bool PrimarySuppressedUntilRelease = false;

		void QueuePrimarySample(PrimaryPointerSample sample);
		void QueueEdit(PresentationEditEvent event);
		uint32_t ControllerIds[scene::MAX_CONTROLLERS] = {};

		// This frame's text, accumulated across however many
		// `SDL_EVENT_TEXT_INPUT` events arrived.
		std::string Typed;

		// The bounded preedit retained across frames until SDL replaces or commits
		// it. It belongs to this platform adapter and is forwarded to GUI as a
		// viewer-local derived value.
		std::string Preedit;
		int32_t PreeditStart = -1;
		int32_t PreeditLength = -1;
		uint64_t PreeditRevision = 0;
	};

	// The key an SDL keycode names, or `Unknown`.
	//
	// **Exported so a test can check the table without an event loop**, which is
	// the only way this mapping gets checked at all: it is ninety-odd entries, and
	// one wrong line is a key that silently does nothing.
	//
	// @param code The SDL keycode.
	// @return The named key.
	scene::KeyCode KeyOf(SDL_Keycode code);
}
