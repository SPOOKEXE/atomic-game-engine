# scriptjs - module invariants

L10, `shared`. **The QuickJS adapter, and every `JSContext *` in this engine.**
`scriptluau`'s twin at the same height and with the same shape: `script` at L9
says what a script may name, and this module is the part that meets the VM.

Read `scriptluau/AGENTS.md` first. Everything it says about what may go in a
module of this kind, and about why neither adapter may name the other, holds here
unchanged and is not repeated.

## What is here that has no Luau twin

**`SourceMap.cpp`, and it is the reason this module has a vendor edge the other
does not.** A `.js.map` is JSON and `Vendor::json` parses it; a `.ts` scene is
transpiled before it ever reaches this VM, so every line number QuickJS knows is
a line in generated JavaScript and `MapStackFrames` rewrites the ones it can.
Nothing on the Luau side needs any of that. It moved here from `script` at v0.19
for the same reason: one caller, and that caller is this VM.

## The microtask queue is the host's, and the drain is bounded

**This is the whole reason a JavaScript VM can live under `world::Driver`.** A
runtime that owned its event loop would resolve promise reactions at a point
nobody chose, which is the desync rule 5 names. `JS_ExecutePendingJob` hands that
decision to the host, so `DrainJobs` runs reactions where the engine says and in
the order the queue holds them.

**A reaction may queue a reaction, so the drain has to have an end.** It did not
until v0.19, and the step budget could not supply one: QuickJS polls the
interrupt handler once per ten thousand safepoints, so a storm of tiny jobs moves
the step counter almost not at all. Measured against the vendored VM, 52 million
jobs and 31,201 polls in two minutes, against a default budget of 200 million -
the host would have got there some time in the next twenty years.

`RuntimeLimits::JobBudget` is the bound. Three properties a reviewer should hold
to:

- **A count and never a deadline.** `just determinism` and `just replay-check`
  are byte-identical only if two runs drain the same number of jobs whatever else
  the machine was doing.
- **The refusal belongs to the script.** It arrives as an ordinary script error
  through `LastError`, not as a host that stopped.
- **The queue is left where it is.** There is no QuickJS call that clears it, and
  pretending otherwise would be worse than the truth: the next call drains at
  most another `JobBudget` and refuses again. *Bounded per tick* is the property
  rule 5 needs.

## One step here is ten thousand safepoints

`Runtime::StepsTaken` on this side counts interrupt polls, and QuickJS raises one
per `JS_INTERRUPT_COUNTER_INIT` safepoints. The divider is fixed, so the number
is the same on every machine - which is the only property either the budget or
the profile panel needs of it. **It is not comparable with Luau's**, and nothing
puts the two in one table: `Costs()` is per runtime.

## `BigInt` is absent because it does not free cleanly

`JS_AddIntrinsicBigInt` over a `JS_NewContextRaw` leaves an object alive and
`JS_FreeRuntime` asserts `list_empty(&rt->gc_obj_list)` on teardown - reproduced
against upstream in isolation, with every other intrinsic in the list clean.
Nothing here needs it. Revisit when something does, and check the teardown again
rather than assuming it was fixed. `JavaScriptRuntime.cpp` carries the rest of
the intrinsic list and why `Date` and `Proxy` are not on it.

## The tests here are deliberately thin

`engine.scriptjs.runtime` asserts that this adapter opens a runtime and builds
into a world **with nothing but `scriptjs` linked** - the one thing the suites in
`scripthost` structurally cannot check, because they link both VMs.
`engine.scriptjs.sourcemap` covers the one piece of behaviour that has no Luau
twin. Everything else belongs in `engine.scripthost.*`, beside Luau's answer to
the same question.
