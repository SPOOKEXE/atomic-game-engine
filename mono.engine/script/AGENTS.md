# script - module invariants

L9. **What a script may name, said without naming a VM.** Above `scene` at L7,
because a script's whole vocabulary is the class tree and the property surface
that module declares; below anything that presents, because a script builds a
world and does not draw one.

## This module links no VM at all

**That is the invariant, and until v0.19 it was a filename convention.** Both
Luau and QuickJS were `VENDOR` on this row, every file in `src/` that reached
`<lua.h>` or `<quickjs.h>` had to be named `Luau*`, `Js*` or `JavaScript*`, and
a sixty-line CMake closure walk called `mono_check_script_vm_naming` re-derived
that transitively on every configure. It worked, and it was the only rule in the
repository shaped like that.

It is a module boundary now, and the closure walk is deleted:

| Module | L | What is in it |
|---|---|---|
| `script` | 9 | the port, the object model glue, the services. **No vendor** |
| `scriptluau` | 10 | the Luau adapter, and every `lua_State *` in the engine |
| `scriptjs` | 10 | the QuickJS adapter, and every `JSContext *` |
| `scripthost` | 11 | one switch on `Language`, and the suites that need both |

A `lua_State` in this directory no longer fails a naming rule. It fails to
compile, because `<lua.h>` is not on the include path - which is what the tier
and layer checks now buy for free, and what rule 6 asks of any constraint.

**Neither adapter may name the other**, which is why the choice lives at L11
rather than in one of them. Two languages over one binding surface means neither
is the real one, and a module that hosted the other would decide that quietly.

**A public header here still may not carry a VM type**, and the reason is now
mechanical rather than moral: `ServiceSurface.hpp` forward-declares `lua_State`
for the `lua_CFunction` rows a service surface holds, and the four functions
that *build* one from those rows - `InstallService`, its two metamethods and
`InstallLuauServiceMethods` - moved to `scriptluau/src/LuauBindings.hpp`. A
declaration taking a `lua_State *` in this module is one nothing here could ever
define.

**A neutral file needing one Luau *number* is not an exception.**
`ServiceProperty` makes a service a userdata in Luau, so `UserInputService.cpp`
and `SoundService.cpp` each name a tag - `LuauTags.hpp` is the tag block on its
own, with no `<lua.h>` under it, and it is a public header of *this* module
because the surface author is the one who writes the number. The same shape
closed `Vocabulary.cpp`: `LuauInstanceSignalNames()` has no VM type in its
signature, so it is declared in `Signals.hpp`.

## Two kinds of file in `src/`, and both compile with no VM present

- **Internal machinery** - runs per tick whether or not a script exists.
  `Tweens.cpp`, `Debris.cpp`, `Bus.cpp`, `Actions.cpp`, `Teleport.cpp`,
  `Signals.cpp`, `Changes.cpp`, `Tasks.cpp`. This is what `AdmitTeleports` has to
  be: a world can be a teleport destination without containing a line of script.
- **The scripting-exposed surface** - `ServiceSurface`, `ScriptMethod`,
  `ServiceProperty` and `ServiceSignal` rows, named for the service a script
  names. `UserInputService.cpp`, `ContextActionService.cpp`, `HttpService.cpp`,
  `TweenService.cpp`, `RunService.cpp` and the rest. Both languages install from
  the one description.

**`ServiceCatalogue.cpp` used to be the one file that had met both VMs and no
longer has.** It hands back rows of data - a name, an availability, a language
mask and one `ServiceSurface` accessor - and each adapter walks them in its own
currency. The linker argument that made it a *table* rather than a registrar per
service file is unchanged and is the reason it must keep naming every accessor:
these are static libraries, and an object file no symbol reaches is one the
archive may drop.

**`Datatypes.cpp` and `PlayerSignals.cpp` are the two files the split created,
and they are worth knowing about because they name the mistake it exposed.**
`RegisterDatatypeEnums`, `DirectionOfAxis`, `DirectionOfNormalId`,
`IsPlayerOfService` and `PlayerLosingCharacter` are declared in this module's
public headers and were *defined* in `LuauDatatypes.cpp` and
`LuauInstances.cpp` - which the old filename rule was perfectly happy with,
because those files do meet `lua.h`. `scriptjs` called all five. That worked
while both VMs were one library and became a link error the moment they were
two. A neutral function defined in an adapter is the shape to refuse.

## An instance is a shim, and there is nothing underneath it

This is the whole model, so it is worth stating without hedging:

> **An instance is an entity. A class is a set of components. A property is a
> projection of one or more of them. Nothing else exists.**

There is no instance object, no per-instance allocation, no table of live
instances, no scripting-only view. The userdata a script holds is an
`ecs::Entity` - an index and a generation, sixty-four bits - and every operation
on it resolves against the same storage a C++ system iterates. `Instance.new`
goes through `Store::CreateInstance`; `part.Size = v` goes through
`Store::SetProperty` and lands in a column. A script and a system are two
callers of one store.

That is why the engine has no transform hierarchy, no dirty cascade and no
scene-graph node: `ecs/Instance.hpp` spends its header on the same point from
the storage side, and Roblox's own model is the reason it works - the tree is
organisational, so parenting moves nothing.

**Two consequences that decide reviews:**

- **A script cannot reach anything C++ cannot.** Adding a property for scripts
  alone is the change to refuse. If a script needs something, the property is
  declared where the components live - `scene`, `examples`, or a game's own
  module - and every consumer of the table gets it at once.
- **The façade must never acquire state of its own.** A cache of instance
  handles, a per-instance flags table, a "script sees this differently" field -
  each of those is a second copy of a fact the store already owns, and rule 2
  exists because two copies drift apart the first time one is written inside a
  branch.

## The marshalling knows types, never names

`LuauInstances.cpp` switches on `PropertyType` and nothing else. No property is
named anywhere in this module, and none ever should be: a property declared in
`scene` tomorrow is readable and writable from Luau today.

A `if (name == "Size")` here would be the second source of truth the whole
conversion design exists to prevent.

## A method is data too, and all forty-one are

A property was neutral from the start and a method was not, and the difference
cost exactly what two lists cost. `ecs::PropertyDescriptor` is data, so `scene`
declares a property once and Luau, JavaScript and the properties panel all read
it. A method was a `lua_CFunction` in `LuauInstances.cpp` and a `JSCFunction` in
`JsSurface.cpp`, written twice - Luau reached thirty entries and JavaScript
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
  than by unwinding - and nothing may escape that frame, since the caller is C.

## A service is data too, and every one of them crossed on it

The same argument one level up. `ServiceSurface` described a service in
`lua_CFunction`s, so it could only build a Luau one - and every JavaScript
service was hand-written, which is how `ContentService`, `CollectionService`,
`HttpService`, `CrossWorldService` and `ContextActionService` came to be
unreachable from JavaScript with the catalogue naming the gap and nothing able
to close it.

`ServiceSurface.hpp` is the answer: `ServiceMethod` carries a `ScriptMethod`, a
service is a name and four lists with no VM in it, and each adapter reads that
one description - `LuauServices.cpp` and `JsServices.cpp`, one walk each over
the rows `ServiceCatalogue.cpp` hands back. A service method is an instance
method whose `Subject()` is `NULL_ENTITY`, so the two adapters gained a
constructor apiece and nothing else.

**A property is not a method, and closing the last two took a second mechanism
rather than more of the first.** `UserInputService` and `SoundService` carry live
values, and the two VMs disagree about which half of that is hard. Luau needs the
service to be a *userdata* - `luaL_sandbox` enables `safeenv`, so a field read off
a constant global **table** compiles to a `GETIMPORT` resolved once and a live
value reads as a frozen one - but could get by with a single `__index` that
string-compares a field name, and did. JavaScript has native accessors that run
on every read and no caching problem at all, but registers one **per name**. So
the catch-all had to become a *list* before either language could stop being the
only one: `ServiceProperty` is a name and two `ScriptMethod`s, Luau's `__index`
walks it and JavaScript defines an accessor per row, and the userdata apparatus -
`Tag`, `MethodsKey` - is unchanged because the trap it defeats has not gone away.

**The last seven crossed on three additions rather than on a rewrite**, and
naming them is the useful half because each is a mechanism the next service will
want. `RunService`, `Debris`, `TweenService` and the bus four were the services
still written twice: `ScriptCall::Await` is how a store method suspends - a
yielded coroutine on one side and a `Promise` on the other, which is the one
thing about them a VM genuinely decides; `ReadFieldNames`/`ReadFieldProperty` are
how `TweenService:Create` reads a goal record whose values are `UDim2`s and
`ColorSequence`s, by *name and declared type*, because `ScriptValue` has no tag
for either and must not gain one; and `ReturnTween` is `ReturnSignal`'s split
again, one entity wrapped per language.

`ContextActionService`'s two reporting methods went the same way and are worth
their own sentence, because they had been described as impossible: a record
holding `Enum.KeyCode` members has no `ScriptValue` form, so the *record* became
a `BoundActionReport` and each adapter builds its own - exactly what
`InputReport` already did for an `InputObject`. The rule that fell out is that a
record with a datatype in it wants a report struct, not a widened wire format.

Five rules a reviewer should hold to:

- **`ServiceSurface::LuauMethods` is down to one service, and that one is not a
  debt.** A row there is a method JavaScript does not have, which was the honest
  shape for a service part way across - `RunService`, `TweenService`, `Debris`,
  the bus four and `ContextActionService`'s two reporting methods each sat there
  and each moved. What is left is `BreakpointService`'s four, whose JavaScript
  half cannot exist for the reason below. A row *added* to that span is a claim
  that a method cannot cross, and it needs the same kind of argument.
- **A property list is data and a property *name* must not appear in an
  adapter.** `LuauServiceIndex` and `InstallJsServiceProperties` walk
  `ServiceSurface::Properties` and know nothing else; a `if (field == "Volume")`
  in either would be the second source of truth the whole arrangement exists to
  prevent, one level up from `LuauInstances.cpp`'s rule about `PropertyType`.
- **A binding is not a pump, and shipping one without the other is the failure
  this module names twice.** `CrossWorldService`'s channel signal needed
  `PumpJsDeliveries` to learn `BusKind::Channel`; `ContextActionService` needed
  a JavaScript input pump to exist at all; `UserInputService`'s six signals
  needed that pump to grow the whole of `PumpInput`'s signal half in the same
  commit. A service installed in a VM that cannot deliver to it is
  `InputChanged` again.
- **A dynamic set of signals is a `Connection::Property` filter, not a signal
  kind each.** `CrossWorldService:OpenChannel(name)` hands back the signal for
  one channel, and both pumps fire only the connections carrying that name - the
  mechanism `GetPropertyChangedSignal` has had since v0.6 and
  `GetAttributeChangedSignal` already reuses for a name the engine never
  declared. The catch-all `MessageReceived` it replaced could not be kept beside
  it: a message on a channel this world never opened is refused at the *bus*, so
  a signal promising "anything addressed to this world" would fire for exactly
  the channels a script had already named, reached by a route that cannot say
  which one arrived. `FireSignal` and `FireJsSignal` take the filter, so a third
  hand-written filtered fire beside `FireInputSignal`'s two was not added.
- **`ScriptValue` is a payload on this interface and never a handle.**
  `ReadValue`/`ReturnValue` carry the tree `HttpService` writes as JSON and
  `CrossWorldService` puts on a bus - values that already leave a world. It has
  no tag for an `EnumItem` and must not gain one. `GetBoundActionInfo` is the
  case that pressed on it: its record holds `Enum.KeyCode` members, and it was
  written twice for two versions on the argument that a return type invented for
  one service's shape is what the interface is not for. What closed it is not a
  widened `ScriptValue` and not a record return - it is
  `ScriptCall::ReturnBoundAction` over a `BoundActionReport`, which is the shape
  `ReturnInputObjects` already had: the *fact* is one struct and only the wrapper
  is two. **`ScriptCall::ReturnEnum` is the same point one size down**: one member
  handed back from a method crosses nothing, so each VM builds its own `EnumItem`
  and no wire format learns about enums.
- **A return is added when a caller asks and never before.** `ReturnVector2`,
  `ReturnEnum`, `ReturnEnums` and `ReturnInputObjects` arrived with
  `UserInputService`'s seven methods; `Role`, `Tweens`, `Debris`, `Subscriptions`,
  `AsTweenInfo`, the two record readers, `ReturnTween`, `ReturnBoundAction`,
  `ForgetSubject` and `Await` arrived with the seven services that stopped being
  written twice. Each names its caller in its own comment, so a member nothing
  calls any more is a member with a lie above it. A pure virtual with no caller
  is a line every adapter has to satisfy the compiler with and nobody has to get
  right.

**`BreakpointService` stays Luau-only for a reason that is not a binding, and it
is the only row that does.** `Debugger::Add` refuses a `.js`, `.mjs`, `.cjs`,
`.ts` or `.tsx` chunk, so a JavaScript binding would answer "nothing can be
armed" to everything - the surface `HttpService`'s absent three are refused for
being. `DEFERRED.md` D00106 carries what closing it would take.

**The shared half of a service is shared machinery, and there are eight of them
now.** `SignalTable`, `ChangeQueue`, `TaskQueue`, `ActionStack`,
`TopicSubscriptions`, `TweenTable`, `DebrisQueue` and `ChildWaiters` each hold an
ordering a recording depends on with the callables left opaque. A service
whose per-language halves differ only in where a list lives is a service whose
list belongs in one of these - `MessagingService` was a Lua registry table on one
side and an `unordered_map` on the other, and neither half decided anything a
language decides.

**Nothing in the catalogue is installed per language any more**, which is what
`ServiceCatalogue.cpp`'s `Row` says by having no `JavaScript` field: every
`Always` row is a `ServiceSurface`, the JavaScript walk installs surfaces and
nothing else, and the one `Luau` pointer left is the studio row's. A pointer
nothing sets is a hole the next service falls into rather than describing itself.

**The nine that moved first are the nine JavaScript did not have**: `GetPivot`,
`PivotTo`, `AddTag`, `RemoveTag`, `HasTag`, `GetAttribute`, `SetAttribute`,
`GetAttributes` and `GetAttributeChangedSignal`. The other twenty-one migrated
one at a time so the engine worked at every step, and **the last twenty crossed
at v0.18**. There is no per-VM instance method table left: `LuauInstances.cpp` keeps
the property surface, the signal branches and `Instance.new`; `JsSurface.cpp`
keeps their JavaScript twins; and the only list either one still builds is
signals. `ScriptMethods.cpp` and `GuiMethods.cpp` are the table, split by what a
method has to reach rather than by size.

**Three of the twenty were not a straight lift, and each was a decision rather
than a chore. All three are answered, and how is worth keeping:**

- **`Destroy` and `ClearAllChildren` release the VM's own callback refs.**
  `ForgetSubtree` takes a lambda that is `lua_unref` on one side and an index
  into `JsContext::Callables` on the other, so the interesting half of both
  methods is per-language by construction. What crosses is an entity and a
  request to forget it, and that is `ScriptCall::Forget` - the `Forget(subtree)`
  shape this file predicted, arriving with the caller that needed it. The *walk*
  is shared now, which is what stops a grandchild's connections outliving the row
  they watched in one language and not the other.
- **`GetPropertyChangedSignal` changed JavaScript's behaviour on the way
  through, and that is a change to state rather than to slip in.** Luau refused a
  name that is not a *scriptable* property and JavaScript compared
  `PropertyDescriptor::Name` and ignored `Scriptable`, so the two disagreed about
  what a script may watch - a JavaScript script could watch `ShaderScript.Source`,
  which the read path refuses by answering "no such member" precisely so an error
  cannot tell a program what is there to reach for. `ScriptableProperty` is the
  one reader now and JavaScript gets the stricter answer.
- **`SetNetworkOwner` took an optional instance and the two refused
  differently.** `CheckInstance` raised for a non-instance where `JsEntityOf`
  answered a null entity for anything at all, which is why the JavaScript half
  carried a hand-written guard the Luau half did not need. `IsNil` then
  `AsInstance` are the two questions it was really asking, and asking them in
  that order is one body that refuses in both.

**One loop replaced three, and finding that was worth more than the migration.**
`LuauInstances.cpp`, `TweenService.cpp` and the JavaScript half of
`GetPropertyChangedSignal` each walked `PropertiesOf` looking for a spelling, and
the third of them had already drifted. `ScriptableProperty` is the one door, and
the rule it carries is the one this module has always stated: **a non-scriptable
property is not found, rather than found and refused.**

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

## The budget counts steps, not seconds - and it is spent once

`RuntimeLimits::StepBudget` bounds an interrupt counter. A wall-clock deadline
would make whether a script finished depend on how busy the machine was, and a
recording made on a fast machine would then replay differently on a slow one -
the desync rule 5 names, arriving through the one mechanism meant to prevent it.

Memory is a hard ceiling through the allocator, so exhaustion surfaces as an
ordinary script error rather than as a `bad_alloc` inside the interpreter.

**One call spends one budget, and both interrupt handlers used to zero their
counter on the trip.** That is not a budget: a `pcall` around a runaway loop
caught the refusal and the next iteration got a whole fresh two hundred million
steps, so a script could run for as long as it liked one trip at a time. Each
runtime now carries a counter that only goes up and a *mark* that every host
entry moves - `Run`, `Heartbeat`, `Invoke`, `Surface`; the budget is the
difference. A reviewer should refuse anything that resets the count rather than
the mark.

**A host entry that does not move the mark is the bug this shape can still
have.** Once tripped a runtime stays tripped, so `Surface` reading a completion
list off a runtime whose last script ran away would have handed back an empty
one. The rule is: if the host can call it and it passes safepoints, it moves the
mark.

**`StepsTaken()` is the same counter and the zeroing lost it**, which made
`Costs()` report nothing for whichever script had just spent the most - the one
the Script Profile panel is opened to find. Two readings are subtracted, so a
counter that goes backwards anywhere reports zero somewhere.

**The step budget cannot bound a microtask queue, and `RuntimeLimits::JobBudget`
is what does.** QuickJS polls its interrupt handler once per ten thousand
safepoints, so a reaction that queues a reaction runs essentially for ever
without moving the step counter: measured against the vendored VM, 52 million
jobs and 31,201 polls in two minutes, against a default budget of 200 million.
The drain is bounded by a *count* of jobs for `StepBudget`'s reason exactly - a
deadline would make `just determinism` and `just replay-check` depend on the
machine. Past the bound the script is refused, the queue is left where it is, and
the next tick drains at most another `JobBudget` of it: bounded per tick is the
property rule 5 needs, and pretending the queue can be emptied is not available.

## A yield is legal only when something is already coming back for it

`v05.md` §5.8 settles what a yield must mean - a script may only resume from
something the barrier delivers in a deterministic order - and `Run` is where that
is enforced: it refuses a suspended thread **that nothing has registered a resume
for**, because a script resumed at some later point nobody chose is work crossing
a tick boundary. `ThreadIsScheduled` is the whole of the test, so the question is
not "did it yield" but "will anything come back", and the answer is a lookup
rather than a judgement.

There are two sources that come back, and each is a table on the context:
`AwaitedTickets` is a bus reply the barrier applied, and `AwaitedChildren` is a
`WaitForChild` the tree answered. A third one is a new table, a new pump and an
argument for why the resume is deterministic - not a convenient default taken
inside whatever needed it.

## An unbounded wait is refused, and that is a divergence from Roblox

`WaitForChild(name, timeout)` is supported. **`WaitForChild(name)` - Roblox's own
form, which waits for ever and warns after five seconds - raises**, with a
message that says why and names the argument to pass.

This is the sharpest place the engine's scheduling argument reaches a script
author, so it is worth stating as a decision rather than leaving in a header. A
wait with no end is a script that never finishes its tick, and the whole of rule
5 is that work does not cross one; the alternatives were to take Roblox's answer
and its consequences, or to refuse the form and diverge from every place that
would be ported in. **The refusal is the honest half**: it costs one line in a
ported script and it is met at the call site, where the two approximations are
met much later -

- a **default timeout** hands a script that ported cleanly a nil it never checks
  for, on a tick nobody chose;
- returning **`FindFirstChild`'s answer immediately** typechecks, works in every
  scene where the child is already there, and answers nil in exactly the case the
  method exists for.

That is `HttpService.cpp`'s and `SoundService.cpp`'s rule - an absent member is
better than one that does nothing - applied to an *argument* rather than to a
member, and `mono.tools/bindings` carries it into both declaration files by
declaring the timeout **required**. So a ported script fails `just typecheck`
before it fails at run time.

**The resume is a second source at the barrier and the first that is not a bus
reply.** `ChildWaiters` is the shared table - a parent, a name, a deadline in
*ticks* through the same `TicksFor` `task.wait` and `Debris` use - and
`PumpChildWaiters` runs immediately after `PumpTree`, so a `ChildAdded` handler
and a woken script see one world. The match is a `FindFirstChild` against the
store rather than a filter over `ecs::TreeChange`, and that is deliberate:
`TakeTreeChanges` is a take, tree changes are only recorded once something calls
the irreversible `ObserveTree`, and a child *renamed* into the awaited name is an
arrival to the author and no reparent at all. `ChildWaiters.hpp` carries all
three.

## The ECS surface names the storage, and refuses to name it twice

`World` and the component methods on every instance are the same store the class
tree sits on, reached without a class. Two refusals in it are the design rather
than gaps to fill in later:

- **A component the engine declared is not readable or writable through
  `GetComponent`/`SetComponent`.** A C++ struct has no field list at run time,
  so there is nothing to marshal a table from - and it already has a property
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
does. That is not a wart to smooth over - it is the model stated in the section
above, and a handle that pretended to have a class would be the scripting-only
view this module does not have.

## A host adds names, and it does it through one seam

`script::HostSurface` is how a *program* offers a script something the engine
does not - a toolbar, a docked panel, the source of another script. The editor
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
  The host table is frozen too - a plugin replacing one of its own host
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
harmless in one direction and not in the other, and `Selection:Set({})` - how a
plugin deselects everything - is the call that was refused before.

**A dotted host name is a service, and `GetService` needed nothing added to
it.** `Selection.Get` becomes a global table with a `Get` method, and
`game:GetService` already resolves a service by looking up a global of the same
name - the property `RunService` has had since v0.6 and whose comment gives the
reason: two objects for one service is two things to keep in step. Both call
forms work, and the binding drops the leading `self` only when it is *that
service's own* table, so `Selection:Set({part})` does not lose its argument.

## A service over a `client` subsystem is a resource on the world

`SoundService` is the case that states the rule and `UserInputService` is the one
that established it. `engine::audio` is L12 `client` and this module is L9
`shared`, so a binding here cannot name a mixer, a graph or a node - the tier
check fails at configure time with the edge named, and it is right to. The seam
is `scene`: a script writes a resource, and whoever owns the device walks it.
`scene::InputState` was the first, `scene::AudioState` is the second, and a third
should look the same rather than inventing a route.

**What the tier decides is scope, not plumbing.** A member needing a *node* the
audio graph does not have cannot be honestly bound however the seam is shaped, so
`SoundService.cpp` names every Roblox member it does not have and what each
would need first - a filter node for reverb, a Doppler node for `DopplerScale`, a
shape in the emitter for `VolumetricAudio`, and for `PlayLocalSound` something
that reports a sound has finished. That list is the file's most useful half, and
it is `HttpService.cpp`'s shape one door along: **an absent member is better than
one that does nothing**, because a member that exists looks decided.

## An input signal carries an `InputObject`, and until v0.16 it carried three things

`InputBegan` fired with a bare `Enum.KeyCode`, a bound action's handler took one
as its third argument, and the generated declarations said both passed *nothing*.
Three answers to one question, none of them Roblox's - so a handler copied from a
Roblox place read `input.KeyCode` off an `EnumItem`, got nil, and typechecked
clean against a declaration that agreed with neither.

That is what a datatype nobody built costs, and it is worth naming because the
same shape is still open one door along: `PumpGuiEvents` passes nothing to a
`TextButton`'s `InputBegan`, and `LuauBindings.hpp` says what closing it needs -
`gui::Router` recording which button produced an event, which is a change in
`gui` rather than here.

**`MouseButton1Click` is `Activated` under Roblox's other name, and one
`SignalKind` serves both.** The router produces exactly one primary button, so
the two questions have one answer here - and a second kind would be a second list
for one event, where whichever name the pump did not know would never fire. That
is the rule for a synonym: **two spellings of one signal, never two lists.**

**`GuiObject.InputChanged` is absent and that is the same rule from the other
side.** Roblox's fires for pointer motion *and* for the wheel over an element;
`gui::Router` produces motion only, and `MouseMoved` already carries it with the
position an argument-less signal could not. Half a member under a familiar name
is what `SoundService.cpp` keeps a list of refusing.

**`engine.script.guisurface` is where a gui signal stops being a claim.** It
stands a world up, lays it out, compiles the list, drives a real `gui::Router`
with a pointer and hands what comes out to `Runtime::DeliverGuiEvents` - so every
one of the seven names above is asserted to have *fired*, in both languages, from
one script. A suite that synthesised a `GuiEvent` would pass against a router
nothing calls, which is exactly the bug that shipped.

**A signal that exists and never fires is the same failure wearing a different
hat.** `InputChanged` was reachable and connectable from v0.10 and nothing ever
fired it; mouse buttons produced no signal at all while `InputState` had carried
their edges since the same version. Both read as a broken engine rather than as
an unfinished one, which is the trade `v0.5` records for `Heartbeat` and the
reason `CollectionService` has no `GetInstanceAddedSignal`.

**The six signals are one `SignalKind` told apart by name, in both languages.**
`ServiceSignal::Property` is what carries the filter, `PumpInput` and
`PumpJsInput` fire the row that matches, and the four report builders -
`KeyReport`, `ButtonReport`, `MotionReport`, `WheelReport` - live in `Actions.cpp`
because two pumps building a report each is two answers to what a frame did. An
engine where a click carried a position in one language and not the other is one
nobody could port a handler between.

**`gameProcessedEvent` is real, and what backs it is the router's own events.**
Roblox's second argument was passing nothing at all, so a handler written
`function(input, gameProcessed)` - the form every Roblox place uses - read nil on
every edge and treated a click on its own menu as a click on the world.
Swallowing the click instead would have been the other wrong answer; the right
one is to deliver it *marked*. So both pumps are handed this beat's
`PendingGuiEvents` and ask `InterfaceHasPointer` once, because an event naming an
element is the only record there is of a press having been taken - `gui::Router`
emits one exactly when the pointer is over or pressed on something that takes
input, and `MouseLeave` is the one kind that means the opposite.

**A key is never game-processed, and that is a gap named rather than a decision.**
`gui` has no keyboard focus: `Router` holds a hover and a press, and a `TextBox`
is a class that draws. Closing it needs the router to hold a focused element and
release it on a press elsewhere, which is a change in `gui` - and until then a key
is honestly unprocessed. `IsPointerReport` is the filter that keeps the answer
from leaking onto one.

**A bound action's return value decides who else hears the key.** The pump called
the winner with `lua_pcall(..., 0, 0)` and a `JS_Call` whose result it freed
unread, so `Enum.ContextActionResult` was unspellable and the case
`ContextActionService` exists for was unsolvable in the interesting direction: a
vehicle nobody is driving wants to let E through to the door it is parked beside.
`ActionStack::ClaimingFrom` is the walk, `Pass` continues it, and `Sink`, nil or a
handler that raised stop it - a raise sinks because handing the key down on the
strength of a crash would make a broken script change which *other* script runs.

**A signal that fires every frame is `InputChanged` wearing the other hat.**
`LastInputTypeChanged` is an edge over `InputState::LastSource`, and the roll that
makes it one lives in `input::Translator::BeginFrame` beside the other three -
`input/AGENTS.md` states it from that side. A version that fired on the value
rather than the change would fire on every frame the player was doing anything.

## A signal about a thing that is destroyed cannot be queued

`Player.CharacterAdded` and `CharacterRemoving` arrive by two different routes,
and the split is forced rather than stylistic - it is `PlayerAdded` and
`PlayerRemoving`'s split, one class along, for a sharper reason.

- **`CharacterAdded` is queued.** `scene::SetPlayerCharacter` records it -
  `scene` is L7 and cannot fire a signal - and `PumpCharacters` drains it at the
  barrier, after `PumpTree`, so a handler indexing `character.Humanoid` sees a
  world whose tree signals have already agreed the model is there.
- **`CharacterRemoving` rides `Store::OnDescendantRemoving`.** Dying in this
  engine *is* the model being destroyed, so a queue drained a tick later hands a
  handler an instance it cannot read a single property off. The first version did
  exactly that and the Luau half raised *"'Name' is not a valid member of this
  instance"* on the second spawn of `engine.script.scriptcall`.

**The two are disjoint and nothing fires twice.** The pump skips a change whose
model is no longer alive, which is every removal the hook already reported; what
is left for the pump is the release that did *not* destroy -
`player.Character = nil` - which the hook cannot see. `PlayerLosingCharacter` is
the filter, and the gate on the *nearest* ancestor is what makes it fire once
rather than once per level of the tree.

**A signal about something being torn down belongs on the hook, not the queue.**
That is the general rule this pair establishes, and `Changes.hpp` already permits
it: nothing is half-written at `OnDescendantRemoving`, because nothing has been
written.

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
  business letting a game script decide - and a service that existed and
  answered "not in a game" to everything is a surface somebody writes against
  and then finds does nothing where it matters.

**Locals and upvalues are captured as two lists and must stay two.** A local is
a value the frame made and an upvalue is one it captured from an enclosing
scope; merging them answers "what is in scope" and loses "where did it come
from", which is the question an upvalue is looked at to answer. A Luau main
chunk closes over nothing - Lua 5.2's `_ENV` upvalue is not Luau's model - so an
empty upvalue list is the ordinary state of a top-level frame and the panel says
so rather than only drawing "none".

**Only Luau has breakpoints, and a chunk that cannot carry one is refused where
somebody asks for it.** `Debugger::Add` answers `false` for a `.js`, `.mjs`,
`.cjs`, `.ts` or `.tsx` chunk, so a dead breakpoint cannot reach the list
through any path - the service, the editor's gutter, the panel, or `Adopt`
copying a list somebody else built. `BreakpointsRefused` is the one function
that decides, and every caller uses it for the message rather than writing its
own.

A breakpoint that sits in the list looking armed and never fires reads as the
debugger being broken rather than as the language not being supported, and those
are different things to go and fix. `D00106` carries what closing the gap would
take; the refusal names it.

**A chunk name with no extension is allowed**, because `Runtime::Run(source,
"probe")` names one that way and it is always Luau - refusing it would refuse
the form every test and every in-editor evaluation uses.

## Timed work happens at the head of the barrier, and it is not a resume

`TweenService` and `Debris` are the first things in this module that move the
world *without a script asking on that tick*, so where they sit in
`LuauRuntime::Heartbeat` is a decision rather than an ordering detail. They run
immediately after the deliveries and before the input pump, and the three rules
that follow are the ones a reviewer should hold to:

- **They step on the fixed tick delta and never on a clock.** A tween that
  advanced by how long the last frame took would put the scene somewhere else on
  a busy machine, and `just replay-check` would fail a long way from the cause -
  the same failure `os` is withheld to prevent. A debris deadline is a *tick
  number*, computed by the same `ceil(seconds / delta)` `task.wait` uses, so
  half a second is thirty ticks at sixty hertz on every machine.
- **Both drain in a stated order and neither may become a hash walk.**
  `TweenTable` is a vector in creation order, so two tweens finishing on one
  tick fire in the order the scripts made them; `DebrisQueue` sorts on
  `(DueTick, Sequence)`, so two items with one deadline go in the order they
  were added. A tween's *goals* are sorted by property name for a third instance
  of the same rule - `Position` and `CFrame` both write `Transform`, so which
  lands last is observable.
- **They go first because everything else in the barrier reacts.** A bound
  action, a `.Changed`, a tree signal, a resumed task and the beat all see one
  world in which this tick's motion has already happened; after the beat
  instead, every script would read a value one tick stale. It also settles what
  a tween made *during* a barrier does - it first advances on the next one.

**A tween is an entity and is deliberately not an instance.** The entity is
minted only to be a name that is unique in a world and can be the subject of a
`SignalTable` entry, which is what makes `Completed` an ordinary
`RBXScriptSignal` in both languages. It carries no class and no components, so
nothing saves it, draws it or replicates it - and `ecs::Classes::Register` is
process-wide, so a registered `Tween` class would have added a row every
consumer of the class table then has to describe, plus an `Instance.new("Tween")`
that mints a tween with no target. `Tweens.hpp` carries the whole argument.

**Its `Play` is a per-VM method rather than a neutral one, and that is the one
place `ScriptCall.hpp` was deliberately not used.** The neutral instance methods
are installed flat on *every* instance, and `Play` is a name Roblox puts on
three classes - claiming it there would take it from every part, sound and
animation in the engine. Three small methods written twice is the cheaper of
the two, and it is what `RBXScriptConnection` already pays.

**`GuiObject`'s three tween methods go the other way, and the difference is the
name.** `TweenPosition`, `TweenSize` and `TweenSizeAndPosition` are neutral rows
in `GuiMethods.cpp`, because none of those three names collides with anything and
each is a `TweenService:Create` a script would otherwise write by hand. They build
`TweenGoal`s through `ScriptCall::ReadProperty` - the argument is read as the
*target's own* `Position` or `Size`, so none of the three names a datatype and
`TweenSize` on a `BasePart` tweens a `Vector3` - and then go through
`TweenTable::Create` and `Play` like everything else. **There is no second
interpolator, no second easing table and no second drain**, which is the rule to
hold to: a convenience method that animated a property itself would be a second
answer to what half way between two `UDim2`s means.

**`override` needed two methods on the table and `callback` needed one on the
interface.** `TweenTable::Driving` and `CancelFor` are what Roblox's flag
actually asks - is something already animating this object, and stop it - and
`ScriptCall::ConnectOnce` is the completion callback, which is a `:Once` on the
tween's `Completed` rather than a second delivery path. Neither is a shortcut: a
`override` that ignored its argument and a `callback` that was read and dropped
are both members that exist and do nothing.

**The *service* is neutral and the handle is not, and the two live in three
files that each say which they are.** `TweenService.cpp` is the surface -
`GetValue`, `Create` and the goal policy, with no VM in it - and `LuauTween.cpp`
and `JsTween.cpp` are the handle. `ScriptCall::ReturnTween` is where they meet,
which is the split `ReturnSignal` was already on: one entity, two wrappers.

**`PumpDebris` is one function and `PumpTweens` is two, which is the same rule
from the other side.** Draining debris destroys instances and fires nothing, so
it takes a store and a queue and both runtimes call it; a tween's `Completed` is
a signal, and calling a callable is the half no shared function can do. A pump
that ends up written twice should be asked which of the two it is.

**Both queues are capped, and they fail in opposite directions on purpose.**
`TweenTable` reclaims the oldest *finished* tween and refuses when every record
is live, because a tween silently dropped is a scene that animates on a small
world and not on a big one. `DebrisQueue` destroys its oldest item early,
because `AddItem` is a cleanup call and tidying up sooner is the conservative
way for one to be wrong. A handle is not a lifetime - nothing here can tell an
unplayed tween somebody still holds from one nobody does - which is why there is
a count at all.

## A program is stored once and mirrored onto the instance for the wire

`SourceCache` is a world resource keyed by path - one table, many instances, and
`ReadSource` consults it before the filesystem. That is right for *storage* and
impossible for the *wire*: `replication::Authority::SetInterest` filters
entities, so a resource can only cross whole, and whole means every client
holding `ServerScriptService`'s programs. `scene::VisibleToClients` exists
because that leak was closed for the instances themselves at v0.15; sending the
table would have re-opened it for their contents.

So `script::Program` is a row on the instance - the path it was read for and the
text - and `MirrorSourcePrograms` fills it from the cache at the head of both
runtimes' beats. Five rules a reviewer should hold to:

- **One writer, and it is derived.** The cache is the record; the row is a
  mirror, exactly as `scene::PreviousTransform` and `gui::Resolved` are mirrors
  of state that lives elsewhere. A second writer would be the two copies rule 2
  forbids.
- **`ReadProgram` reads the cache, then the row, then the disk, and the order is
  the load-bearing part.** The cache wins wherever it has an answer, so an
  editor's unsaved edit still runs; the row is what a replica has *instead* of a
  cache; the filesystem is last, so a client cannot quietly run a file of its own
  in place of the program it was sent.
- **Only what a client could run is mirrored.** A `LocalScript` and a
  `ModuleScript`, never a `Script` - so a server's program is not a component at
  all, and no interest predicate has to be right for it to stay on the server.
- **It does nothing in a replica.** `Store::AdoptOnly` is the test, as it is for
  `OpenWorkspace`: the rows arrived from the authority, and refilling them from
  whatever this machine has on disk is how a client runs a program the server
  never sent. The window is real - a container can land a tick before the program
  beside it.
- **The cost of a tick that edited nothing is a `SourceCache::Generation`
  compare and a name compare per script.** The counter is bumped by `Set` and
  `Erase` and is deliberately not serialised. Nothing reads a program's bytes
  unless the generation moved, and a rebuild still compares before writing, so
  one file saved in an editor does not put every program in the world back on the
  wire. Measured at 29 µs a tick over fifty scripts of a kilobyte at `-O0`,
  against 200 µs for a hash of the same programs - which is what a
  `ChangeDetection::Signature` over them would have cost every tick to learn that
  nobody had typed anything.

**`LuaSourceContainer` and its JavaScript twin write their path as text**, which
they did not until v0.15. A `core::Name` is a process-local counter, so the
object representation put an interning index into a save file and would have put
one on a wire - `ecs::DescribeType`'s warning names exactly this hazard.
`ecs::Store::SNAPSHOT_VERSION` 4 and `replication::PROTOCOL_VERSION` 9 refuse the
old encoding rather than misreading it.

## One runtime, one world

The `Store` is an upvalue on every bound function rather than a global, so two
runtimes over two worlds cannot reach each other's storage. A file-static would
have made that mistake available, and it is the sort that works until the second
world exists.

## A client's replica runs scripts, and the store is what refuses them

`ClientScriptsIn` is this module's answer to "which of a world's scripts may a
client run when it does not own the world". Three rules a reviewer should hold
to:

- **It is a second selection, not an argument on the first.** `ScriptsIn`
  answers Roblox's class rule - a `Script` is the server's, a `LocalScript` is a
  client's - and that is the whole answer for a host that owns its world. A
  replica needs the container rule as well: a `LocalScript` runs when it is under
  the local player's own subtree or under `ReplicatedFirst`. Folding that into
  `ScriptsIn` would stop a `LocalScript` an author parked in `Workspace` from
  running in single player, which is a change to how every existing scene loads
  for the sake of a rule about somebody else's world.
- **No refusal is written here, and none should be.** A client script cannot
  write a property and cannot mint an instance, and both are `ecs::Store`'s
  refusals through `AdoptOnly` - `SetProperty` and `MayMintAuthoritative`. What
  this module owes is that the refusal *reaches the author*:
  `InstanceNewIndex` raises with a message naming the world as a replica, because
  a script author cannot tell a write that was rejected from one that was applied
  and then overwritten by the next delta.
- **A replica is never asked to furnish itself.** `OpenWorkspace` and its
  JavaScript twin call `scene::InstallServices` only when the store may mint;
  otherwise they resolve whatever the authority has already sent. Asking anyway
  worked - the mint is refused and the fallback finds the arrived `Workspace` -
  and logged "refusing CreateInstance" at error level on every join, which reads
  as a fault and is the ordinary state of a world that owns nothing.

`Runtime::RunNewScripts` is the other half. `RunWorldScripts` is a host saying
"start this game" and starts everything it finds, every time it is called; a
replica fills from the wire, so what it has to ask each tick is what arrived.
The record of what has already started is the runtime's own - two VMs over one
world would each answer it separately - which is why it is a member rather than
a tag on the row.
