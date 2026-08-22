# input - module invariants

L12, `client` tier. Absent from the server binary entirely, and from a
server-only configure altogether.

## This module answers "what is held". It does not decide what a key means

`KeyOf` in `Translate.cpp` is the only place in the engine where an SDL keycode
becomes a `scene::KeyCode`, and that is the whole of what this module owns: a
name a script can read, on a resource a world holds.

**`Action` and `Actions` left at v0.19, and the argument is worth keeping.**
They were here until then, and `docs/ARCH_REVIEW.md` C6 was right about them:
all twelve members were the host program's own diagnostics - ten profiler and
HUD panels, a wireframe toggle and `Quit` - and exactly one target in the
repository linked this module for them. A game does not ask whether
`WriteProfilerSnapshot` fired; it reads `scene::InputState`. So they are
`mono.client/include/client/Actions.hpp` now, beside the profiler and the HUD
they drive.

The rule that used to live here said "nothing outside this module names a key",
and moving the enum did not break it, because what it protects is that a
*binding* is one table rather than branches spread over a codebase. There are
still exactly two places an `SDLK_` appears: `KeyOf` here, and `BINDINGS` in
`mono.client`. **Do not add a third**, and in particular do not scatter
`event.key.key == SDLK_...` through a frame loop.

If the engine ever needs a game-facing action map, it is a different thing from
what moved - it is bindings a game declares at runtime, not an enum - and it
belongs here, over `scene::KeyCode` rather than over `SDL_Keycode`.

## This module translates events. It does not pump them

The program owns its event loop, because some platforms insist the loop live on
the thread that created the window. `Actions::HandleEvent` takes one event and
returns whether a binding consumed it.

Do not add a `Poll()` here. It would work on Linux and fail on macOS in a way
that looks like an input bug.

## Edges and levels are different questions

`Fired` is edge-triggered and lasts exactly one frame - "the player pressed
F5". `Held` is level-triggered - "the player is holding forward". A toggle read
through `Held` fires every frame it is down, which reads as the panel
flickering rather than as a wrong API.

Autorepeat is dropped: the OS turning a held key into a stream of presses is
not the player expressing an intent many times.

## `BeginFrame` rolls four fields, and every edge a script sees is one of them

`Translator::BeginFrame` copies `Down`, `Buttons`, `Focused` and `LastSource`
into their `Previous` twins and clears the two deltas. That is not four
housekeeping lines - it is the whole of `InputBegan`, `InputEnded`,
`WindowFocused`, `WindowFocusReleased` and `LastInputTypeChanged`, because every
one of those is `scene::InputState` comparing a pair.

**A fifth field read as an edge needs a fifth roll here**, and forgetting it
produces a signal that fires on every frame the player is doing anything -
which reads as a broken engine rather than as an unfinished one.
`engine.input.translate` presses twice and compares, because a suite that only
looks at the live half passes either way.

**Anything `HandleEvent` consumes also stamps `LastSource`, and a focus change
deliberately does not.** Losing a window is not a device speaking, and a place
that swapped its prompts on an alt-tab would be reporting a keyboard nobody
touched. A key *release* does stamp it, or a place flickers every time somebody
stops walking.

## Text is a sixth field, and it is cleared rather than rolled

`SDL_EVENT_TEXT_INPUT` is what a keystroke *spelled*, where a key event is which
key moved. The two arrive together and are not duplicates: the layout, the
modifiers and any input method sit between them, so `Shift` and `1` is two key
bits in one and `!` in the other.

`Translator::TypedText` accumulates it and `BeginFrame` clears it. **It is not
rolled**, and that is the distinction the section above is about: a key that is
down has a previous value to be an edge against, and a character was produced
once. `ReleaseAll` drops it with the other deltas, because a frame that ended
with the window going away did not finish delivering.

Two conventions the build cannot check:

- **UTF-8, so one byte is not one character.** The event's text is appended
  whole. Anything that indexes it, truncates it or takes a byte at a time cuts a
  codepoint in half the first time somebody types in their own language.
- **The string lives here rather than on `scene::InputState`**, where every
  other frame delta lives. `InputState` is a registered trivially-copyable
  component and a `std::string` on it would owe a hand-written serialiser, so
  the one reader takes it from here instead: `client::Client` hands
  `TypedText()` to `gui::Type` once a frame, which writes the world's own
  `Label::Text`. The delta never crosses a snapshot at all, which is why it did
  not have to move.

**SDL sends none of these until a host calls `SDL_StartTextInput`**, which is
the platform's rule and not this module's: text input raises an on-screen
keyboard and starts composition, so it is off until something asks. The client
asks while a `TextBox` has the keyboard and stops when it does not - that call
belongs where the window is, and this module has no window.

## An unmapped key is reported, not silent

`KeyOf` answers `KeyCode::Unknown` for anything not in its switch, and
`Translator::HandleEvent` then drops the event. That is correct - `KeyCode` only
names keys this engine can produce, which is `scene/Input.hpp`'s own rule - but
it used to be invisible, and the symptom is a script that cannot see a key the
player can see on their keyboard.

Since v0.19 the drop is a `debug` line under the `input` category carrying the
SDL keycode, so `--log input=debug` names the key that would have to be added to
the table. Keep it that way: if a key is worth mapping, the log is where the
request comes from.
