// What translating a busy frame of platform input costs.
//
// Four rows isolate the key, mouse, controller-axis and frame-roll paths. All
// events are built before timing, so the figures only include translation.

#include <engine/input/Translate.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

TEST_SUITE_ID("engine.input.bench.translate")
TEST_DEPENDS("engine.input.translate")

using engine::input::Translator;
using engine::testing::Consume;

namespace translate_bench {
	constexpr size_t KEY_EVENTS = 102'400;
	constexpr size_t GAMEPAD_AXES = 8;
	constexpr size_t GAMEPAD_EVENTS = GAMEPAD_AXES * 2;
	constexpr uint32_t GAMEPAD_ID = 1;
	constexpr std::array<uint8_t, GAMEPAD_AXES> AXIS_KINDS = {
		SDL_GAMEPAD_AXIS_LEFTX,
		SDL_GAMEPAD_AXIS_LEFTY,
		SDL_GAMEPAD_AXIS_RIGHTX,
		SDL_GAMEPAD_AXIS_RIGHTY,
		SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
		SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
		SDL_GAMEPAD_AXIS_LEFTX,
		SDL_GAMEPAD_AXIS_LEFTY,
	};

	// The fixture is first built during a warm-up sample. The timed body only
	// translates events that have already been constructed.
	struct Fixture {
		std::array<SDL_Event, KEY_EVENTS> Keys{};
		SDL_Event Mouse{};
		std::array<SDL_Event, GAMEPAD_EVENTS> Axes{};
		Translator KeysInput;
		Translator MouseInput;
		Translator AxesInput;
		Translator FrameInput;

		Fixture() {
			for (size_t index = 0; index < Keys.size(); index++) {
				SDL_Event &event = Keys[index];
				event.type = index % 2 == 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
				event.key.key = SDLK_W;
			}

			Mouse.type = SDL_EVENT_MOUSE_MOTION;
			Mouse.motion.x = 640.0f;
			Mouse.motion.y = 360.0f;
			Mouse.motion.xrel = 2.0f;
			Mouse.motion.yrel = -1.0f;

			for (size_t index = 0; index < Axes.size(); index++) {
				SDL_Event &event = Axes[index];
				event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
				event.gaxis.which = GAMEPAD_ID + static_cast<uint32_t>(index % GAMEPAD_AXES);
				event.gaxis.axis = AXIS_KINDS[index % GAMEPAD_AXES];
				event.gaxis.value = index < GAMEPAD_AXES ? 32767 : -32767;
			}

			for (size_t index = 0; index < GAMEPAD_AXES; index++) {
				SDL_Event added{};
				added.type = SDL_EVENT_GAMEPAD_ADDED;
				added.gdevice.which = GAMEPAD_ID + static_cast<uint32_t>(index);
				if (!AxesInput.HandleEvent(added)) std::abort();
			}

			// FrameInput starts with every field BeginFrame rolls or clears non-zero.
			// The benchmark then measures the roll alone.
			SDL_Event held{};
			held.type = SDL_EVENT_KEY_DOWN;
			held.key.key = SDLK_W;
			if (!FrameInput.HandleEvent(held) || !FrameInput.HandleEvent(Mouse)) std::abort();
		}
	};

	Fixture &Events() {
		static Fixture fixture;
		return fixture;
	}
}

using namespace translate_bench;

BENCH("Translator::HandleEvent · 102,400 alternating keys", KEY_EVENTS) {
	Fixture &fixture = Events();

	size_t consumed = 0;
	for (const SDL_Event &event : fixture.Keys) {
		consumed += fixture.KeysInput.HandleEvent(event) ? 1u : 0u;
	}
	if (consumed != KEY_EVENTS || fixture.KeysInput.State().IsKeyDown(engine::scene::KeyCode::W))
		std::abort();
	Consume(consumed);
}

BENCH("Translator::HandleEvent · mouse motion", KEY_EVENTS) {
	Fixture &fixture = Events();

	size_t consumed = 0;
	for (size_t index = 0; index < KEY_EVENTS; index++) {
		consumed += fixture.MouseInput.HandleEvent(fixture.Mouse) ? 1u : 0u;
	}
	if (consumed != KEY_EVENTS || fixture.MouseInput.State().MousePosition.X != 640.0f) std::abort();
	Consume(consumed);
	Consume(fixture.MouseInput.State().MouseDelta.X);
}

BENCH("Translator::HandleEvent · controller axes", KEY_EVENTS) {
	Fixture &fixture = Events();

	size_t consumed = 0;
	for (size_t index = 0; index < KEY_EVENTS; index++) {
		const SDL_Event &event = fixture.Axes[index % GAMEPAD_EVENTS];
		consumed += fixture.AxesInput.HandleEvent(event) ? 1u : 0u;
	}
	for (const engine::scene::ControllerSlot &slot : fixture.AxesInput.Controllers().Slots) {
		if (!slot.Connected) std::abort();
	}
	const auto &lastSlot = fixture.AxesInput.Controllers().Slots[GAMEPAD_AXES - 1];
	if (consumed != KEY_EVENTS ||
		lastSlot.Axes[static_cast<size_t>(engine::scene::ControllerAxis::LeftY)] != -1.0f) {
		std::abort();
	}
	Consume(consumed);
	Consume(lastSlot.Axes[static_cast<size_t>(engine::scene::ControllerAxis::LeftY)]);
}

BENCH("Translator::BeginFrame · roll", KEY_EVENTS) {
	Fixture &fixture = Events();

	for (size_t index = 0; index < KEY_EVENTS; index++) {
		fixture.FrameInput.BeginFrame();
	}
	if (!fixture.FrameInput.State().IsKeyDown(engine::scene::KeyCode::W) ||
		fixture.FrameInput.State().MouseDelta.X != 0.0f || fixture.FrameInput.State().MouseDelta.Y != 0.0f) {
		std::abort();
	}
	Consume(fixture.FrameInput.State().IsKeyDown(engine::scene::KeyCode::W));
}
