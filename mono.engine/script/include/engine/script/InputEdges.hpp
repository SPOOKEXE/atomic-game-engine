#pragma once

#include <engine/scene/characters/Input.hpp>

#include <cstdint>

namespace engine::script {

	// The keys and mouse buttons a script runtime last reported as down.
	//
	// Scripts pump input once per tick, while hosts write `InputState` once per
	// frame and roll `Previous` each time. Frames outnumber ticks, so an edge
	// taken as `Down` against `Previous` is lost whenever the frame it happened
	// on ran no tick. Taking edges against what this runtime last reported
	// cannot lose one: a key held across several frames still begins once,
	// on the first pump that sees it.
	struct InputEdges {
		scene::KeyBits Seen;
		uint8_t SeenButtons = 0;

		// Whether `input` holds a key or button this runtime has not reported.
		bool Pending(const scene::InputState &input) const {
			if (input.Buttons != SeenButtons) return true;
			for (size_t word = 0; word < sizeof(Seen.Words) / sizeof(Seen.Words[0]); word++)
				if (input.Down.Words[word] != Seen.Words[word]) return true;
			return false;
		}

		bool Began(const scene::InputState &input, scene::KeyCode key) const {
			return input.Down.Has(key) && !Seen.Has(key);
		}
		bool Ended(const scene::InputState &input, scene::KeyCode key) const {
			return !input.Down.Has(key) && Seen.Has(key);
		}
		bool Began(const scene::InputState &input, scene::MouseButton button) const {
			const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
			return (input.Buttons & bit) != 0 && (SeenButtons & bit) == 0;
		}
		bool Ended(const scene::InputState &input, scene::MouseButton button) const {
			const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
			return (input.Buttons & bit) == 0 && (SeenButtons & bit) != 0;
		}

		// Records `input` as reported. Called once the pump has fired.
		void Report(const scene::InputState &input) {
			Seen = input.Down;
			SeenButtons = input.Buttons;
		}
	};
}
