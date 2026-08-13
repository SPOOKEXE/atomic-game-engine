# script — module invariants

L9. Running a script against a world. Above `scene` at L7, because a script's
whole vocabulary is the class tree and the property surface that module
declares; below anything that presents, because a script builds a world and does
not draw one.

## The VM never leaves this directory

No `lua_State` appears in a public header. `Runtime.hpp` forward-declares it and
holds a pointer, so nothing outside compiles against Luau and no consumer can
call the VM directly.

That is what makes `v05.md` §5.7's second VM a source file here rather than a
second module: JavaScript arrives beside Luau over the same `Runtime` shape,
and every caller keeps working. The moment a vendor type reaches a public
header, that stops being true — which is why the CMake row says `VENDOR` and not
`VENDOR_PUBLIC`.

## An instance is a shim, and there is nothing underneath it

This is the whole model, so it is worth stating without hedging:

> **An instance is an entity. A class is a set of components. A property is a
> projection of one or more of them. Nothing else exists.**

There is no instance object, no per-instance allocation, no table of live
instances, no scripting-only view. The userdata a script holds is an
`ecs::Entity` — an index and a generation, sixty-four bits — and every operation
on it resolves against the same storage a C++ system iterates. `Instance.new`
goes through `Store::CreateInstance`; `part.Size = v` goes through
`Store::SetProperty` and lands in a column. A script and a system are two
callers of one store.

That is why the engine has no transform hierarchy, no dirty cascade and no
scene-graph node: `ecs/Instance.hpp` spends its header on the same point from
the storage side, and Roblox's own model is the reason it works — the tree is
organisational, so parenting moves nothing.

**Two consequences that decide reviews:**

- **A script cannot reach anything C++ cannot.** Adding a property for scripts
  alone is the change to refuse. If a script needs something, the property is
  declared where the components live — `scene`, `examples`, or a game's own
  module — and every consumer of the table gets it at once.
- **The façade must never acquire state of its own.** A cache of instance
  handles, a per-instance flags table, a "script sees this differently" field —
  each of those is a second copy of a fact the store already owns, and rule 2
  exists because two copies drift apart the first time one is written inside a
  branch.

## The marshalling knows types, never names

`Instances.cpp` switches on `PropertyType` and nothing else. No property is
named anywhere in this module, and none ever should be: a property declared in
`scene` tomorrow is readable and writable from Luau today.

A `if (name == "Size")` here would be the second source of truth the whole
conversion design exists to prevent.

## A method is data too, and nine of thirty are

A property was neutral from the start and a method was not, and the difference
cost exactly what two lists cost. `ecs::PropertyDescriptor` is data, so `scene`
declares a property once and Luau, JavaScript and the properties panel all read
it. A method was a `lua_CFunction` in `Instances.cpp` and a `JSCFunction` in
`JsSurface.cpp`, written twice — Luau reached thirty entries and JavaScript
twenty-one, nothing in the build named the nine that were missing, and the
TypeScript declarations claimed all nine anyway.

`ScriptCall.hpp` is the answer: a method is a name and a `void(ScriptCall &)`,
`ScriptMethods.cpp` is the table, and `LuauCall.cpp` and `JsCall.cpp` are the
only two files that meet a VM on their behalf. **Both runtimes install every
row**, which is what makes parity a property of the build rather than of
somebody comparing two files, and `engine.script.scriptcall` runs the same
script in each and compares the answers.

Four rules a reviewer should hold to:

- **`ScriptValue` is not the currency and merging the two would be the change to
  refuse.** `ValueTag` has no instance and `CodecStatus::Unsupported` is what a
  script gets for offering one, because a `ScriptValue` crosses a *world* and
  rule 3 forbids a handle naming a row in one store from arriving in another. A
  method call crosses nothing, so `ecs::Entity` is exactly right here and
  exactly wrong there. This is the same split `HostValue` is on, one door along.
- **The interface carries what its callers ask for and nothing else.** There is
  no `ReturnInstance` and no optional-instance reader today, because none of the
  nine takes or returns one. A pure virtual with no caller is a line every
  adapter has to satisfy the compiler with and nobody has to get right; adding
  one when the first method needs it is a change the build refuses to let
  anybody forget.
- **A row must not be installed by one VM only.** That is the drift the layer
  exists to close, and re-opening it is what a hand-written entry beside the
  table would do.
- **A reader raises and never answers a failure**, so a method body reads its
  arguments straight through. On the JavaScript side that costs a thrown type
  the trampoline catches, because QuickJS reports an error by returning rather
  than by unwinding — and nothing may escape that frame, since the caller is C.

**The nine that moved are the nine JavaScript did not have**: `GetPivot`,
`PivotTo`, `AddTag`, `RemoveTag`, `HasTag`, `GetAttribute`, `SetAttribute`,
`GetAttributes` and `GetAttributeChangedSignal`. The other twenty-one stay where
they are and migrate one at a time, so the engine works at every step.

**Three of the twenty-one are not a straight lift, and each is a decision rather
than a chore:**

- **`Destroy` and `ClearAllChildren` release the VM's own callback refs.**
  `ForgetSubtree` takes a lambda that is `lua_unref` on one side and an index
  into `JsContext::Callables` on the other, so the interesting half of both
  methods is per-language by construction. What crosses is an entity and a
  request to forget it, which wants a `Forget(subtree)` on `ScriptCall` rather
  than a return.
- **`GetPropertyChangedSignal` would change JavaScript's behaviour on the way
  through.** Luau refuses a name that is not a *scriptable* property and
  JavaScript compares `PropertyDescriptor::Name` and ignores `Scriptable`, so
  the two already disagree about what a script may watch. Migrating it fixes
  that, which is a good change and not a silent one.
- **`SetNetworkOwner` takes an optional instance, and the two refuse differently
  today.** `CheckInstance` raises for a non-instance where `JsEntityOf` answers
  a null entity for anything at all, which is why the JavaScript half carries a
  hand-written guard the Luau half does not need. One reader would settle it,
  and settling it is a behaviour change to state rather than to slip in.

## What a script may not do is the design

Opened without **`os`** and without **`debug`**, and both are refusals rather
than an oversight:

- `os` is `time`, `clock` and `date`. A script branching on a wall clock
  produces a run that does not replay, and `just replay-check` would fail a long
  way from the cause. A world's clock is `store.Time()`, and it is simulated.
- `debug` reaches stack frames, locals and upvalues. A game loads scripts it did
  not write; that is the library for climbing out of a sandbox.

Globals are frozen with `luaL_sandbox`, so one script cannot rewrite the
language the next one runs in, and each chunk runs on a sandboxed thread so its
globals are its own. The value-type and `Instance` metatables set
`__metatable`, so a script cannot reach in and replace `__newindex` for every
instance in the world.

**If you add a library, the question is what it can observe that a recording
cannot reproduce.** That is the test, not whether it looks dangerous.

## The budget counts steps, not seconds

`RuntimeLimits::StepBudget` bounds an interrupt counter. A wall-clock deadline
would make whether a script finished depend on how busy the machine was, and a
recording made on a fast machine would then replay differently on a slow one —
the desync rule 5 names, arriving through the one mechanism meant to prevent it.

Memory is a hard ceiling through the allocator, so exhaustion surfaces as an
ordinary script error rather than as a `bad_alloc` inside the interpreter.

## Nothing yields yet, and a yield is an error

`v05.md` §5.8 settles what a yield must mean — a script may only resume from
something the barrier delivers in a deterministic order — and the bus surface a
script would yield *on* is v0.6. Until then `Run` refuses a suspended thread
rather than finishing the tick with one, because a script resumed at some later
point nobody chose is work crossing a tick boundary.

Do not make this "work" by resuming on the next tick. That is the design
decision v0.6 has to take deliberately, and a convenient default taken here is
how it would get taken by accident.

## The ECS surface names the storage, and refuses to name it twice

`World` and the component methods on every instance are the same store the class
tree sits on, reached without a class. Two refusals in it are the design rather
than gaps to fill in later:

- **A component the engine declared is not readable or writable through
  `GetComponent`/`SetComponent`.** A C++ struct has no field list at run time,
  so there is nothing to marshal a table from — and it already has a property
  surface. Adding a byte-level path to `scene::Visual` would be two ways to
  write one component, which the root `AGENTS.md` calls the most expensive kind
  of debt. `HasComponent` answers for any component, because asking is not
  reaching.
- **A query naming a component nothing declared is an error, not an empty
  result.** A typo would otherwise be a loop that never runs, and a loop that
  never runs reads exactly like a world with nothing in it.

`World:CreateEntity` makes a **bare** entity: no class, no place in the tree,
nothing drawn. It is still an `Instance` on the script side because an instance
*is* an entity, and reading `.Name` on one fails the way any missing member
does. That is not a wart to smooth over — it is the model stated in the section
above, and a handle that pretended to have a class would be the scripting-only
view this module does not have.

## A host adds names, and it does it through one seam

`script::HostSurface` is how a *program* offers a script something the engine
does not — a toolbar, a docked panel, the source of another script. The editor
adds `CreateDockWidget` and nothing in this module changes, which is the whole
point of the shape.

Four rules a reviewer should hold to:

- **`HostValue` is not `ScriptValue` widened, and merging them is the change to
  refuse.** `ScriptValue` is what crosses a *world boundary*, where rule 3 says
  everything is a copy and a handle means nothing; `HostValue` carries an
  `Instance` precisely because a host call is inside one process against one
  store. Adding an instance tag to the first would make the wrong thing
  expressible on a bus.
- **The global is built from `Names()`, not answered by an `__index`.** A name
  the host does not list is not a member, so a typo is "attempt to call a nil
  value" at the call site rather than a refusal from inside a program the author
  cannot see.
- **The globals are unfrozen for exactly one assignment.** `SetHost` runs after
  `luaL_sandbox`, so the table is readonly and a plain `lua_setglobal` throws.
  The host table is frozen too — a plugin replacing one of its own host
  functions would be replacing it for every later chunk in that VM.
- **`ReadHostValue` grows the stack before it recurses.** A C function is
  guaranteed `LUA_MINSTACK` slots and a map traversal holds a key and a value
  per level; overrunning it is a `LUAU_ASSERT`, so the symptom was an illegal
  instruction from a script that merely nested a table.

**A refusal is a message and never an abort.** `Call` answers `false` with a
reason and the script sees an ordinary error, because a host that aborted would
take the program down with a plugin's typo.

**An empty Luau table crosses as an `Array`, not a `Map`.** `{}` is one value
and the reader has to pick a tag; a host expecting a map finds no entries under
either, where a host expecting a *list* gets a tag it refuses. The ambiguity is
harmless in one direction and not in the other, and `Selection:Set({})` — how a
plugin deselects everything — is the call that was refused before.

**A dotted host name is a service, and `GetService` needed nothing added to
it.** `Selection.Get` becomes a global table with a `Get` method, and
`game:GetService` already resolves a service by looking up a global of the same
name — the property `RunService` has had since v0.6 and whose comment gives the
reason: two objects for one service is two things to keep in step. Both call
forms work, and the binding drops the leading `self` only when it is *that
service's own* table, so `Selection:Set({part})` does not lose its argument.

## A service over a `client` subsystem is a resource on the world

`SoundService` is the case that states the rule and `UserInputService` is the one
that established it. `engine::audio` is L12 `client` and this module is L9
`shared`, so a binding here cannot name a mixer, a graph or a node — the tier
check fails at configure time with the edge named, and it is right to. The seam
is `scene`: a script writes a resource, and whoever owns the device walks it.
`scene::InputState` was the first, `scene::AudioState` is the second, and a third
should look the same rather than inventing a route.

**What the tier decides is scope, not plumbing.** A member needing a *node* the
audio graph does not have cannot be honestly bound however the seam is shaped, so
`SoundService.cpp` names the eleven Roblox members it does not have and what each
would need first — a filter node for reverb, a Doppler node for `DopplerScale`, a
shape in the emitter for `VolumetricAudio`, and for `PlayLocalSound` something
that reports a sound has finished. That list is the file's most useful half, and
it is `HttpService.cpp`'s shape one door along: **an absent member is better than
one that does nothing**, because a member that exists looks decided.

## An input signal carries an `InputObject`, and until v0.16 it carried three things

`InputBegan` fired with a bare `Enum.KeyCode`, a bound action's handler took one
as its third argument, and the generated declarations said both passed *nothing*.
Three answers to one question, none of them Roblox's — so a handler copied from a
Roblox place read `input.KeyCode` off an `EnumItem`, got nil, and typechecked
clean against a declaration that agreed with neither.

That is what a datatype nobody built costs, and it is worth naming because the
same shape is still open one door along: `PumpGuiEvents` passes nothing to a
`TextButton`'s `InputBegan`, and `Bindings.hpp` says what closing it needs —
`gui::Router` recording which button produced an event, which is a change in
`gui` rather than here.

**A signal that exists and never fires is the same failure wearing a different
hat.** `InputChanged` was reachable and connectable from v0.10 and nothing ever
fired it; mouse buttons produced no signal at all while `InputState` had carried
their edges since the same version. Both read as a broken engine rather than as
an unfinished one, which is the trade `v0.5` records for `Heartbeat` and the
reason `CollectionService` has no `GetInstanceAddedSignal`.

## The debugger captures, and `BreakpointService` is the same object

`Debugger.hpp` carries the argument for why a breakpoint records rather than
pauses; two rules about the surface around it are this file's:

- **`BreakpointService` writes the runtime's own `Debugger` and never a second
  list.** The editor's panel writes `Runtime::Debug()` directly and a script
  writes the service; a service that kept breakpoints of its own would be two
  things to keep in step and a breakpoint that fired in one place and not the
  other.
- **It is installed only when `RuntimeLimits::Role::Studio` is set, and it is
  absent rather than refusing.** Arming a breakpoint switches Luau's step mode
  on and costs the whole runtime its speed, which a shipped server has no
  business letting a game script decide — and a service that existed and
  answered "not in a game" to everything is a surface somebody writes against
  and then finds does nothing where it matters.

**Locals and upvalues are captured as two lists and must stay two.** A local is
a value the frame made and an upvalue is one it captured from an enclosing
scope; merging them answers "what is in scope" and loses "where did it come
from", which is the question an upvalue is looked at to answer. A Luau main
chunk closes over nothing — Lua 5.2's `_ENV` upvalue is not Luau's model — so an
empty upvalue list is the ordinary state of a top-level frame and the panel says
so rather than only drawing "none".

**Only Luau has breakpoints, and a chunk that cannot carry one is refused where
somebody asks for it.** `Debugger::Add` answers `false` for a `.js`, `.mjs`,
`.cjs`, `.ts` or `.tsx` chunk, so a dead breakpoint cannot reach the list
through any path — the service, the editor's gutter, the panel, or `Adopt`
copying a list somebody else built. `BreakpointsRefused` is the one function
that decides, and every caller uses it for the message rather than writing its
own.

A breakpoint that sits in the list looking armed and never fires reads as the
debugger being broken rather than as the language not being supported, and those
are different things to go and fix. `D00106` carries what closing the gap would
take; the refusal names it.

**A chunk name with no extension is allowed**, because `Runtime::Run(source,
"probe")` names one that way and it is always Luau — refusing it would refuse
the form every test and every in-editor evaluation uses.

## Timed work happens at the head of the barrier, and it is not a resume

`TweenService` and `Debris` are the first things in this module that move the
world *without a script asking on that tick*, so where they sit in
`LuauRuntime::Heartbeat` is a decision rather than an ordering detail. They run
immediately after the deliveries and before the input pump, and the three rules
that follow are the ones a reviewer should hold to:

- **They step on the fixed tick delta and never on a clock.** A tween that
  advanced by how long the last frame took would put the scene somewhere else on
  a busy machine, and `just replay-check` would fail a long way from the cause —
  the same failure `os` is withheld to prevent. A debris deadline is a *tick
  number*, computed by the same `ceil(seconds / delta)` `task.wait` uses, so
  half a second is thirty ticks at sixty hertz on every machine.
- **Both drain in a stated order and neither may become a hash walk.**
  `TweenTable` is a vector in creation order, so two tweens finishing on one
  tick fire in the order the scripts made them; `DebrisQueue` sorts on
  `(DueTick, Sequence)`, so two items with one deadline go in the order they
  were added. A tween's *goals* are sorted by property name for a third instance
  of the same rule — `Position` and `CFrame` both write `Transform`, so which
  lands last is observable.
- **They go first because everything else in the barrier reacts.** A bound
  action, a `.Changed`, a tree signal, a resumed task and the beat all see one
  world in which this tick's motion has already happened; after the beat
  instead, every script would read a value one tick stale. It also settles what
  a tween made *during* a barrier does — it first advances on the next one.

**A tween is an entity and is deliberately not an instance.** The entity is
minted only to be a name that is unique in a world and can be the subject of a
`SignalTable` entry, which is what makes `Completed` an ordinary
`RBXScriptSignal` in both languages. It carries no class and no components, so
nothing saves it, draws it or replicates it — and `ecs::Classes::Register` is
process-wide, so a registered `Tween` class would have added a row every
consumer of the class table then has to describe, plus an `Instance.new("Tween")`
that mints a tween with no target. `Tweens.hpp` carries the whole argument.

**Its `Play` is a per-VM method rather than a neutral one, and that is the one
place `ScriptCall.hpp` was deliberately not used.** The neutral instance methods
are installed flat on *every* instance, and `Play` is a name Roblox puts on
three classes — claiming it there would take it from every part, sound and
animation in the engine. Three small methods written twice is the cheaper of
the two, and it is what `RBXScriptConnection` already pays.

**Both queues are capped, and they fail in opposite directions on purpose.**
`TweenTable` reclaims the oldest *finished* tween and refuses when every record
is live, because a tween silently dropped is a scene that animates on a small
world and not on a big one. `DebrisQueue` destroys its oldest item early,
because `AddItem` is a cleanup call and tidying up sooner is the conservative
way for one to be wrong. A handle is not a lifetime — nothing here can tell an
unplayed tween somebody still holds from one nobody does — which is why there is
a count at all.

## One runtime, one world

The `Store` is an upvalue on every bound function rather than a global, so two
runtimes over two worlds cannot reach each other's storage. A file-static would
have made that mistake available, and it is the sort that works until the second
world exists.
