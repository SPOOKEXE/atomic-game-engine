# input — module invariants

L12, `client` tier. Absent from the server binary entirely, and from a
server-only configure altogether.

## One table names keys. Nothing else does

`BINDINGS` in `Actions.cpp` is the only place in the engine where a key code
appears. Everything else asks whether an action fired.

That is what makes rebinding a table edit rather than a search, and it is why
`SDLK_F5` must not appear in `mono.client`. If a new behaviour needs a key, it
needs an `Action` first.

## This module translates events. It does not pump them

The program owns its event loop, because some platforms insist the loop live on
the thread that created the window. `Actions::HandleEvent` takes one event and
returns whether a binding consumed it.

Do not add a `Poll()` here. It would work on Linux and fail on macOS in a way
that looks like an input bug.

## Edges and levels are different questions

`Fired` is edge-triggered and lasts exactly one frame — "the player pressed
F5". `Held` is level-triggered — "the player is holding forward". A toggle read
through `Held` fires every frame it is down, which reads as the panel
flickering rather than as a wrong API.

Autorepeat is dropped: the OS turning a held key into a stream of presses is
not the player expressing an intent many times.

## `BeginFrame` rolls four fields, and every edge a script sees is one of them

`Translator::BeginFrame` copies `Down`, `Buttons`, `Focused` and `LastSource`
into their `Previous` twins and clears the two deltas. That is not four
housekeeping lines — it is the whole of `InputBegan`, `InputEnded`,
`WindowFocused`, `WindowFocusReleased` and `LastInputTypeChanged`, because every
one of those is `scene::InputState` comparing a pair.

**A fifth field read as an edge needs a fifth roll here**, and forgetting it
produces a signal that fires on every frame the player is doing anything —
which reads as a broken engine rather than as an unfinished one.
`engine.input.translate` presses twice and compares, because a suite that only
looks at the live half passes either way.

**Anything `HandleEvent` consumes also stamps `LastSource`, and a focus change
deliberately does not.** Losing a window is not a device speaking, and a place
that swapped its prompts on an alt-tab would be reporting a keyboard nobody
touched. A key *release* does stamp it, or a place flickers every time somebody
stops walking.

## Every action needs a binding and a name

`GetActionBinding` feeds the overlay, so an action with no binding string is a
feature nobody can discover. The test asserts both exist for every action in
the enum, which is why adding one to the enum and forgetting the table is a
test failure rather than a silent gap.
