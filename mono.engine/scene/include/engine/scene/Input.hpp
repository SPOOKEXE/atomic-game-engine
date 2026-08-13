#pragma once

// What the player is doing, as a world can see it.
//
// **This exists because `input` is `client` and `script` is `shared`.**
// `engine::input` owns the SDL event pump and sits at L12; a script binding is at
// L9 and may not name it. So `UserInputService` cannot read the module that
// produces its data — the state has to come to rest somewhere both can see, and
// that place is a resource on the world.
//
// The shape follows from that. **This module holds no SDL type and pumps
// nothing**: the client translates events and writes here, exactly as it walks
// `scene::Sound` rows and drives `engine::audio`. `scene` is where a fact about
// the world lives; who produced it is somebody else's business.
//
// **Keys are named, never scancoded.** `KeyCode` is Roblox's `Enum.KeyCode` in
// spelling and its ordinals are this file's own — a script says
// `Enum.KeyCode.Space` and never a number, and the number is free to move because
// nothing writes it down. The one thing that *would* pin it is a saved keybinding
// file, and there is not one; when there is, it saves names.
//
// **A server has one of these and it is empty, which is the point.** A world
// ticks the same systems whoever hosts it, so a character controller reading
// input compiles and runs on a headless server and simply finds nothing pressed.
// What crosses from a real client is `replication`'s business and not this
// file's.
//
// @tier L7 · shared

#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>

#include <cstdint>
#include <string_view>

namespace engine::scene {

	// A key, by name.
	//
	// **Roblox's `Enum.KeyCode` names, and a subset of them.** Every member here
	// is one the client's translation layer can produce; a name that mapped to
	// nothing would be offering an author completion for a key that never fires.
	//
	// The ordinals are this file's own and are free to move — nothing serialises
	// one. Scripts say `Enum.KeyCode.Space`.
	//
	// @since v0.10
	enum class KeyCode : uint16_t {
		Unknown = 0,

		A,
		B,
		C,
		D,
		E,
		F,
		G,
		H,
		I,
		J,
		K,
		L,
		M,
		N,
		O,
		P,
		Q,
		R,
		S,
		T,
		U,
		V,
		W,
		X,
		Y,
		Z,

		Zero,
		One,
		Two,
		Three,
		Four,
		Five,
		Six,
		Seven,
		Eight,
		Nine,

		Space,
		Return,
		Escape,
		Backspace,
		Tab,
		LeftShift,
		RightShift,
		LeftControl,
		RightControl,
		LeftAlt,
		RightAlt,

		Up,
		Down,
		Left,
		Right,

		F1,
		F2,
		F3,
		F4,
		F5,
		F6,
		F7,
		F8,
		F9,
		F10,
		F11,
		F12,

		// Not a key. The count, for the bitset below.
		Count,
	};

	// A mouse button or a wheel, in the same space as a key.
	//
	// **Separate from `KeyCode` rather than appended to it**, which is Roblox's
	// split and is the one that keeps `IsKeyDown` honest: a caller asking whether
	// a key is down should not be able to pass a mouse button and get an answer.
	//
	// @since v0.10
	enum class MouseButton : uint8_t {
		Left = 0,
		Right = 1,
		Middle = 2,

		// Not a button. The count.
		Count,
	};

	// How the pointer behaves while the game has focus.
	//
	// @since v0.10
	enum class MouseBehavior : uint8_t {
		// Free to move and visible. The ordinary case.
		Default = 0,

		// Held at the centre and hidden, which is what a first-person or
		// shift-locked camera needs: the pointer stops existing and its *motion*
		// becomes the camera's input.
		LockCenter = 1,

		// Held where it is and hidden. What a drag-to-rotate gesture wants, so
		// the pointer is back where it started when the drag ends.
		LockCurrentPosition = 2,
	};

	// Every key and button, as bits.
	//
	// **A bitset rather than an array of bools**, because this is copied into a
	// snapshot and compared frame to frame: 96 keys as bytes is 96 bytes and as
	// bits is 12, and the frame-to-frame compare that produces `InputBegan` is
	// then a handful of integer XORs rather than a loop.
	//
	// @since v0.10
	struct KeyBits {
		// One bit per `KeyCode`, in ordinal order.
		uint64_t Words[(static_cast<size_t>(KeyCode::Count) + 63) / 64] = {};

		// Whether a key's bit is set.
		//
		// @param key The key.
		// @return `true` when it is down.
		bool Has(KeyCode key) const {
			const auto index = static_cast<size_t>(key);
			return index < static_cast<size_t>(KeyCode::Count) &&
				   (Words[index / 64] & (1ull << (index % 64))) != 0;
		}

		// Sets or clears a key's bit.
		//
		// @param key  The key.
		// @param down Whether it is down.
		void Set(KeyCode key, bool down) {
			const auto index = static_cast<size_t>(key);
			if (index >= static_cast<size_t>(KeyCode::Count)) {
				return;
			}
			const uint64_t bit = 1ull << (index % 64);
			if (down) {
				Words[index / 64] |= bit;
			} else {
				Words[index / 64] &= ~bit;
			}
		}

		// Reports whether any bit is set.
		//
		// @return `true` when at least one key is down.
		bool Any() const {
			for (const uint64_t word : Words) {
				if (word != 0) {
					return true;
				}
			}
			return false;
		}
	};

	// What the player is doing this frame.
	//
	// A resource: there is one player at a keyboard and nothing iterates them.
	//
	// **Both `Down` and `Previous` are kept**, because the difference between them
	// is what `InputBegan` and `InputEnded` are. Deriving the edges where the
	// events arrive would put the answer in the client, where a script cannot
	// reach it, and would make "was this key down last frame" a question only one
	// module could ask.
	//
	// @since v0.10
	struct InputState {
		// Which keys are down now.
		KeyBits Down;

		// Which were down when the previous frame's input was written.
		KeyBits Previous;

		// Which keys have gone down since a tick last consumed the edges.
		//
		// **The frame/tick mismatch, made a field instead of a workaround.**
		// `Down` and `Previous` answer "was this pressed *this frame*", and a
		// frame is the wrong unit for anything the simulation acts on: frames
		// outnumber ticks, so a key tapped and released between two ticks was
		// pressed on a frame no tick ever looked at. A jump read that way is
		// dropped about two times in three, and both hosts had independently
		// grown the same private latch to hide it — `PlayLink::PendingJump` and
		// `client::Client::PendingJump`, each bypassing `InputState` entirely
		// and each having to be wired to its own character by hand.
		//
		// **Sticky until `ConsumeTaps`**, which is what makes it correct rather
		// than merely longer-lived: the writer ORs every frame's press edge in
		// here, and the tick that acts on it clears it. Nothing is read twice
		// and nothing is missed.
		//
		// `WasKeyPressed` deliberately does *not* consult this — a script's
		// `InputBegan` is a per-frame event and must stay one. `WasKeyTapped`
		// is the tick-shaped question.
		KeyBits Pressed;

		// Where the pointer is, in pixels from the top-left of the window.
		core::Vector2 MousePosition;

		// How far it moved since the last frame, in pixels.
		//
		// **Carried rather than derived from two positions**, because under
		// `LockCenter` the position does not change — the pointer is held at the
		// centre and only the motion is real. A camera that differenced positions
		// would stop turning the moment it locked, which is exactly when it needs
		// to turn.
		core::Vector2 MouseDelta;

		// How far the wheel has turned since the last frame, in notches.
		float WheelDelta = 0.0f;

		// Which mouse buttons are down, one bit each.
		uint8_t Buttons = 0;

		// Which were down last frame.
		uint8_t PreviousButtons = 0;

		// How the pointer should behave.
		//
		// **Written by a script and read by the client**, which is the one field
		// here that travels in that direction. `UserInputService.MouseBehavior` is
		// a property an author sets, and the client applies it to the window on
		// the next frame — so this resource is the seam in both directions rather
		// than a one-way report.
		MouseBehavior Behaviour = MouseBehavior::Default;

		// Whether the window has keyboard focus.
		//
		// **Everything reads as released when it does not**, and the client is
		// what enforces that: alt-tabbing away while holding W must not leave a
		// character walking forever, which is the bug every engine ships once.
		bool Focused = true;

		// Explicit padding, so the object representation a snapshot writes holds
		// no uninitialised bytes.
		//
		// **Eight and not two, and the difference is the whole point of naming
		// it.** The members above end at 42 and the type aligns to 8, so two
		// bytes left six the compiler inserted and nobody declared — written to
		// a save file by `Column::Write`, which sends `sizeof(T)` bytes and does
		// not know which of them a member claimed. `Reserved` has to reach the
		// end or it is not doing the job it is here for; this is the only
		// component in the module where it did not, and the rest are the reason
		// the rule is worth keeping.
		//
		// `sizeof` is 64 since `Pressed` joined the two key sets above it — a
		// save written before that reads its input resource back wrong, which
		// is a break the pre-release format is allowed and the dropped jump was
		// not.
		uint8_t Reserved[8] = {};

		// Whether a key is down now.
		//
		// @param key The key.
		// @return `true` when it is down.
		bool IsKeyDown(KeyCode key) const {
			return Down.Has(key);
		}

		// Whether a key went down since the previous frame.
		//
		// @param key The key.
		// @return `true` on the frame it was pressed.
		bool WasKeyPressed(KeyCode key) const {
			return Down.Has(key) && !Previous.Has(key);
		}

		// Whether a key came up since the previous frame.
		//
		// @param key The key.
		// @return `true` on the frame it was released.
		bool WasKeyReleased(KeyCode key) const {
			return !Down.Has(key) && Previous.Has(key);
		}

		// Records this frame's press edges, so a tick can still see them.
		//
		// **Called by whoever writes `Down`, once per frame, after it is
		// filled.** It is the writer's job rather than the reader's because
		// only the writer knows a frame happened: a reader that latched on its
		// own would collapse two presses in one tick into one.
		void LatchPresses() {
			for (size_t word = 0; word < sizeof(Down.Words) / sizeof(Down.Words[0]); word++) {
				Pressed.Words[word] |= Down.Words[word] & ~Previous.Words[word];
			}
		}

		// Whether a key went down since a tick last consumed the edges.
		//
		// **What a simulation should ask instead of `WasKeyPressed`.** A tap
		// that began and ended between two ticks is invisible to the frame-shaped
		// question and answered correctly by this one.
		//
		// **The latch alone, and deliberately not `|| WasKeyPressed(key)`.**
		// Folding the live frame edge in here would put a bit in the answer that
		// `ConsumeTaps` cannot clear, and ticks are not rationed to one per
		// frame: a host catching up runs several between two writes, and the
		// second would read the same press the first had just consumed. One
		// press, two jumps. `LatchPresses` already folds the current frame's
		// edge into `Pressed`, so nothing is lost by asking only the latch.
		//
		// @param key The key.
		// @return `true` when it was pressed since the last `ConsumeTaps`.
		bool WasKeyTapped(KeyCode key) const {
			return Pressed.Has(key);
		}

		// Forgets the latched press edges.
		//
		// **Once per tick, by the one consumer that acts on them.** Two callers
		// would mean the second never sees a tap the first cleared, which is the
		// dropped jump again with an extra step.
		void ConsumeTaps() {
			Pressed = KeyBits{};
		}

		// Whether a mouse button is down now.
		//
		// @param button The button.
		// @return `true` when it is down.
		bool IsButtonDown(MouseButton button) const {
			return (Buttons & (1u << static_cast<uint8_t>(button))) != 0;
		}

		// Whether a mouse button went down since the previous frame.
		//
		// @param button The button.
		// @return `true` on the frame it was pressed.
		bool WasButtonPressed(MouseButton button) const {
			const uint8_t bit = 1u << static_cast<uint8_t>(button);
			return (Buttons & bit) != 0 && (PreviousButtons & bit) == 0;
		}

		// Whether a mouse button came up since the previous frame.
		//
		// @param button The button.
		// @return `true` on the frame it was released.
		bool WasButtonReleased(MouseButton button) const {
			const uint8_t bit = 1u << static_cast<uint8_t>(button);
			return (Buttons & bit) == 0 && (PreviousButtons & bit) != 0;
		}
	};

	// The name a key is known by.
	//
	// **Round-trips**, unlike `Describe(BodyKind)` — these are the names
	// `ecs::EnumTable` registers and a script compares against, so they are the
	// surface rather than a diagnostic.
	//
	// @param key The key.
	// @return A view valid for the lifetime of the process.
	const char *Describe(KeyCode key);

	// The key a name refers to.
	//
	// @param name The name, as `Describe` spells it.
	// @return The key, or `KeyCode::Unknown`.
	KeyCode KeyFromName(std::string_view name);

	// The name a mouse button is known by.
	//
	// @param button The button.
	// @return A view valid for the lifetime of the process.
	const char *Describe(MouseButton button);
}
