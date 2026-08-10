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

## One runtime, one world

The `Store` is an upvalue on every bound function rather than a global, so two
runtimes over two worlds cannot reach each other's storage. A file-static would
have made that mistake available, and it is the sort that works until the second
world exists.
