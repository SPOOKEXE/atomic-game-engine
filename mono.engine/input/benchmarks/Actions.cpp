// What translating an input event costs, on the path a player can feel.
//
// **This is the shortest latency chain in the engine and the only one a human
// perceives directly.** Everything else here is measured against a frame or a
// tick; this is measured against the moment somebody presses a key and the
// moment the world reacts. The engine's share of that is small by construction
// — `Actions` is two fixed-size arrays and a switch — but "small by
// construction" is a claim, and a claim about the input path is the one worth
// having a number for, because a regression in it reads to a player as the game
// feeling worse without anything being visibly wrong.
//
// **The event counts are chosen against a real input pump, not against a
// stress test.** A keyboard produces a handful of events a frame; a mouse in
// motion produces dozens; a mouse in motion on a 1000 Hz polling rate produces
// hundreds. The rows below cover that range, and the important one is the
// *unbound* row: an event pump hands `Actions` every event SDL produced, and
// the overwhelming majority of them are motion events it does not care about.
// So the cost of saying "not mine" is paid far more often than the cost of
// saying "mine", and a translation layer that got that backwards would be slow
// exactly when the player was moving.
//
// Nothing here initialises SDL. `SDL_Event` is a plain tagged union and
// `HandleEvent` reads it and nothing else, so the events are built as structs —
// which is also the only way this module can be measured at all on a machine
// with no display.

#include <engine/input/Actions.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.input.bench.actions")

using engine::input::Action;
using engine::input::Actions;
using engine::testing::Consume;

namespace actions_bench {

	// Events per row. Two hundred is a frame of a high-polling-rate mouse being
	// moved hard, which is the busiest an input pump realistically gets.
	constexpr size_t EVENTS = 200;

	// A key event for a scancode the default bindings use.
	//
	// Built by hand rather than pumped, because pumping needs a window and a
	// display and this module's cost has nothing to do with either.
	SDL_Event KeyEvent(SDL_Scancode scancode, bool down, bool repeat) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.scancode = scancode;
		event.key.down = down;
		event.key.repeat = repeat;
		return event;
	}

	// A mouse motion event, which is the kind an event pump produces most of and
	// which `Actions` does not consume.
	SDL_Event MotionEvent(float x, float y) {
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.x = x;
		event.motion.y = y;
		event.motion.xrel = 1.0f;
		event.motion.yrel = 1.0f;
		return event;
	}

	// The scancodes the default bindings cover, discovered rather than assumed.
	//
	// **Read out of `GetActionBinding` rather than hard-coded**, so the
	// benchmark cannot drift from the bindings it is measuring: a rebound action
	// changes what this presses without anybody remembering to update a list
	// here. Falls back to WASD when a binding does not name a single key.
	const std::vector<SDL_Scancode> &BoundKeys() {
		static const std::vector<SDL_Scancode> keys = [] {
			std::vector<SDL_Scancode> found;
			for (size_t index = 0; index < static_cast<size_t>(Action::Count); index++) {
				const std::string_view binding = engine::input::GetActionBinding(static_cast<Action>(index));
				const SDL_Scancode scancode =
					binding.empty() ? SDL_SCANCODE_UNKNOWN
									: SDL_GetScancodeFromName(std::string(binding).c_str());
				if (scancode != SDL_SCANCODE_UNKNOWN) {
					found.push_back(scancode);
				}
			}
			if (found.empty()) {
				found = {SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D};
			}
			return found;
		}();
		return keys;
	}
}

using namespace actions_bench;

// --- the common case ---------------------------------------------------------------

BENCH("HandleEvent · 200 unbound motion events", EVENTS) {
	// **The row that runs most often.** An event pump hands `Actions` everything
	// SDL produced, and while a player is moving, nearly all of it is motion.
	// Rejecting an event `Actions` does not care about must be the cheapest thing
	// in the module — if it is not, the input layer is slowest precisely when the
	// player is most active.
	static Actions actions;
	static const SDL_Event motion = MotionEvent(100.0f, 100.0f);

	uint32_t consumed = 0;
	for (size_t index = 0; index < EVENTS; index++) {
		consumed += actions.HandleEvent(motion) ? 1u : 0u;
	}
	Consume(consumed);
}

BENCH("HandleEvent · 200 bound key transitions", EVENTS) {
	// Down then up on every bound key, over and over. Each one has to find the
	// action for the scancode and set two bits, so this is the whole of what the
	// module does when it does something.
	static Actions actions;
	const std::vector<SDL_Scancode> &keys = BoundKeys();

	uint32_t consumed = 0;
	for (size_t index = 0; index < EVENTS; index++) {
		const SDL_Scancode scancode = keys[index % keys.size()];
		consumed += actions.HandleEvent(KeyEvent(scancode, (index & 1u) == 0u, false)) ? 1u : 0u;
	}
	Consume(consumed);
}

BENCH("HandleEvent · 200 key repeats", EVENTS) {
	// A held key at the OS repeat rate. **A repeated key-down for a bound action
	// is consumed without firing the action again**, which is the behaviour that
	// stops a held key firing sixty times a second — and the check that
	// implements it runs on every repeat, so it belongs in the measurement.
	static Actions actions;
	const std::vector<SDL_Scancode> &keys = BoundKeys();

	uint32_t consumed = 0;
	for (size_t index = 0; index < EVENTS; index++) {
		consumed += actions.HandleEvent(KeyEvent(keys[index % keys.size()], true, true)) ? 1u : 0u;
	}
	Consume(consumed);
}

BENCH("HandleEvent · 200 unbound key events", EVENTS) {
	// Keys nothing is bound to — the whole of the keyboard a game does not use,
	// plus anything the player has rebound away. Read against the bound row: the
	// gap is what looking a binding up costs, and a *large* gap would mean the
	// lookup is a search rather than a table.
	static Actions actions;

	uint32_t consumed = 0;
	for (size_t index = 0; index < EVENTS; index++) {
		// The function keys, which no default binding uses.
		const auto scancode = static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (index % 12));
		consumed += actions.HandleEvent(KeyEvent(scancode, true, false)) ? 1u : 0u;
	}
	Consume(consumed);
}

// --- reading the result --------------------------------------------------------------

BENCH("Fired and Held · 100k queries", 100'000) {
	// Read by whatever consumes input, once per action per frame at least, and
	// often more than once — a camera controller and a character controller both
	// ask about the same movement actions. It is an array index and must stay
	// one.
	static Actions actions;
	uint32_t active = 0;
	for (size_t index = 0; index < 100'000; index++) {
		const auto action = static_cast<Action>(index % static_cast<size_t>(Action::Count));
		active += actions.Fired(action) ? 1u : 0u;
		active += actions.Held(action) ? 1u : 0u;
	}
	Consume(active);
}

BENCH("BeginFrame · 100k frames", 100'000) {
	// **Clears fired edges while preserving held states**, once per frame. It
	// touches one array of `Action::Count` bytes, so it should be a single
	// memset-sized operation — this row exists so that an `Actions` that grew a
	// per-action structure would show the growth here rather than in a frame
	// time nobody could attribute.
	static Actions actions;
	for (size_t index = 0; index < 100'000; index++) {
		actions.BeginFrame();
	}
	Consume(actions.Held(static_cast<Action>(0)));
}

// --- a frame ---------------------------------------------------------------------------

BENCH("frame · BeginFrame then 64 mixed events", 1000) {
	// **One input frame, the way a client runs it**: clear the edges, then pump
	// whatever arrived — mostly motion, a few key transitions. One iteration is
	// one frame, so this is the input layer's whole per-frame share and it should
	// be a rounding error against a 16.7 ms budget.
	//
	// If it ever is not, the fix is upstream in the pump rather than here: this
	// module cannot be handed fewer events than SDL produced.
	static Actions actions;
	static const SDL_Event motion = MotionEvent(64.0f, 64.0f);
	const std::vector<SDL_Scancode> &keys = BoundKeys();

	for (size_t frame = 0; frame < 1000; frame++) {
		actions.BeginFrame();

		uint32_t consumed = 0;
		for (size_t event = 0; event < 64; event++) {
			if ((event % 16) == 0) {
				consumed +=
					actions.HandleEvent(KeyEvent(keys[event % keys.size()], (frame & 1u) == 0u, false)) ? 1u : 0u;
			} else {
				consumed += actions.HandleEvent(motion) ? 1u : 0u;
			}
		}
		Consume(consumed);
	}
}
