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

## Every action needs a binding and a name

`GetActionBinding` feeds the overlay, so an action with no binding string is a
feature nobody can discover. The test asserts both exist for every action in
the enum, which is why adding one to the enum and forgetting the table is a
test failure rather than a silent gap.
