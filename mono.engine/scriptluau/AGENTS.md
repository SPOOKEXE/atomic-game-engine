# scriptluau - module invariants

L10, `shared`. **The Luau adapter, and every `lua_State *` in this engine.**
Above `script` at L9, because everything a script can *name* - the class tree,
the property surface, the services, the signals, the tick - is described there
with no VM in it. What is here is the part that meets the VM.

## What this module is allowed to hold, and what it is not

**In:** anything that takes, returns or stores a `lua_State *`, a
`lua_CFunction`, a userdata tag in use, a registry ref, or a Luau bytecode
buffer. That is the whole test, and it is now the compiler's rather than a
reviewer's: `script` does not have `<lua.h>` on its include path.

**Out:** anything that would compile with no VM present. Until v0.19 the rule
that kept the two apart was a filename convention checked by a sixty-line CMake
closure walk in `script/CMakeLists.txt`, and it let five neutral functions -
`RegisterDatatypeEnums`, `DirectionOfAxis`, `DirectionOfNormalId`,
`IsPlayerOfService`, `PlayerLosingCharacter` - live in `LuauDatatypes.cpp` and
`LuauInstances.cpp` while `scriptjs` called all five. That was legal under the
old rule and is a link error under this one. **A neutral function defined here is
the shape to refuse**; it belongs in `script`, where its declaration already is.

## This module may not name `scriptjs`, and `scriptjs` may not name this one

They are two modules at one height with no edge between them, and there is no
`lateral` entry for either. That is deliberate rather than incidental: "two
languages, two VMs, one binding surface" is a claim that neither language is the
real one, and a module that hosted the other would settle that question by
accident. The choice lives in `scripthost` at L11 and nowhere else.

The practical form of the rule: if a fact is needed by both adapters, it goes
*down* into `script`, not sideways.

## The budget is a mark, not a counter that resets

`Interrupt` increments `Bounds::StepsTaken` and never resets it; every host
entry - `Run`, `Heartbeat`, `Invoke`, `Surface` - moves `Bounds::StepsBase`, and
the budget is the difference. **A host entry that forgets to move the mark is
the bug this shape can still have**: a tripped runtime stays tripped, so
`Surface` would have answered a completion list with nothing in it. Zeroing the count on the trip - which is what it did until v0.19 -
was two bugs at once: a `pcall` around a runaway loop caught the refusal and the
next iteration got a whole fresh budget, and `Runtime::StepsTaken` lost exactly
the script that had spent the most, because `Costs()` subtracts two readings.

Counted rather than timed, for the reason `script/AGENTS.md` gives at length: a
wall-clock deadline makes whether a script finished depend on how busy the
machine was, and a recording stops replaying.

## A file here is half of a pair, or it says why it is not

`LuauInput.cpp`/`JsInput.cpp`, `LuauTween.cpp`/`JsTween.cpp`,
`LuauServiceSurface.cpp`/`JsServiceSurface.cpp`, `LuauServices.cpp`/
`JsServices.cpp`. Where a fact is shared and only the wrapper is not, the shared
half is in `script` and the two wrappers are named for each other, so a member
added to one has an obvious place to be added to the other.

**`LuauDebugService.cpp` is the one file with no twin, and that is a feature gap
rather than a binding one.** `Debugger::Add` refuses a `.js`, `.mjs`, `.cjs`,
`.ts` or `.tsx` chunk outright, so a JavaScript `BreakpointService` would answer
"nothing can be armed" to everything. Closing it means teaching QuickJS to report
a line, which is `DEFERRED.md` D00106.

**The catalogue's studio row is dispatched by name in `LuauServices.cpp`**, and
that is the price of a VM-free catalogue: a `void (*)(lua_State *)` on the row
would have put Luau back in a header `scriptjs` also reads. A second name in that
dispatch is a second claim that a service cannot describe itself, and it needs
the same kind of argument written beside it.

## The tests here are deliberately thin

`engine.scriptluau.runtime` asserts that this adapter opens a runtime and builds
into a world **with nothing but `scriptluau` linked**, which is the one thing the
suites in `scripthost` structurally cannot check: they link both VMs, so an edge
that crossed to the other adapter would still pass every one of them. The
*behaviour* of a Luau runtime belongs in `engine.scripthost.*`, where it is
asserted against JavaScript's at the same time and in the same case.
