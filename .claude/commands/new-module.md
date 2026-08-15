---
description: Scaffold a new mono.engine module with its tier, tests, AGENTS.md and graph entry.
argument-hint: <name> [layer]
---

Add a new engine module called `$1`.

A module is not just a directory. Six things have to line up, and missing any
one of them produces something that builds today and is wrong later. Do all six.

## Before writing anything

Answer these, out loud, in your response. If any answer is unclear, ask rather
than guess - a module at the wrong height is expensive to move once things
depend on it.

1. **What layer does it sit at?** A layer may see every layer below it and none
   above. Check the existing modules' heights in `mono.engine/CMakeLists.txt`,
   which lists them in layer order.
2. **What tier?** `shared` for simulation, data and protocol. `client` for
   presentation and input. `server` for authority and hosting. If the honest
   answer is "both client and server need it", it is `shared`.
3. **Does it earn a module?** A module boundary is worth it when the thing has
   a surface smaller than its contents, or when it is a security boundary, or
   when a program needs to link it without the rest. "It is a lot of code" is
   not a reason on its own.
4. **What does it deliberately not depend on?** This becomes the first section
   of its `AGENTS.md`, and it is the useful half.

## Then create

```
mono.engine/$1/
├─ CMakeLists.txt              mono_add_library with TIER and DEPS
├─ AGENTS.md                   the invariants - see below
├─ include/engine/$1/          public headers, and only public headers
├─ src/                        sources and private headers
├─ tests/                      one file per public header
└─ shaders/                    only if it owns GLSL, and only if client tier
```

`CMakeLists.txt` is normally four lines:

```cmake
mono_add_library($1
	TIER <shared|client|server>
	DEPS Engine::core
)
```

Tests are picked up automatically when `tests/` exists. Do not write an
`add_executable` for them.

## Then wire it in

**`mono.engine/CMakeLists.txt`** - add the `add_subdirectory` in layer order,
with the layer and tier in the trailing comment like its neighbours. If it is
`client` tier it goes inside the `MONO_BUILD_CLIENT` guard; if `server`, it
needs the mirror-image guard.

**`mono.tools/architecture/expected_graph.json`** - add the entry. A new module
is an architectural change and should show up in review as a diff to that file.
If it is behind a build option, give it `"requires": "<OPTION>"`, or the
architecture test will fail under the preset that turns the option off.

The `links` list is the transitive closure of first-party modules, sorted.
Configure once and read the real answer out of
`.cache/build/dev/target-graph.json` rather than working it out by hand.

## The AGENTS.md

The root `AGENTS.md` carries policy. This one carries invariants - the things
that are true about this module and would be broken by a reasonable-looking
change. Write it as what a reviewer should refuse, not as a description.

Look at `mono.engine/core/AGENTS.md` and `mono.engine/ecs/AGENTS.md` for the
shape. Cover at least:

- what may go in, and what may not, with the reason
- anything the module deliberately does not depend on, and why
- any place the obvious implementation is wrong
- anything named in the design notes that is not here yet, so nobody adds half
  of one

An `AGENTS.md` that only describes what the module does is not worth reading.

## Then verify

```sh
cmake --preset dev && cmake --build .cache/build/dev -j
cmake --preset server && cmake --build .cache/build/server -j
( cd .cache/build/dev && ctest --output-on-failure )
```

Both presets, because a `client`-tier module that a `shared` one accidentally
links will only fail in the server-only configure.

Then check the tier rule actually holds for the new edge: it is enforced by
`mono_check_all_tiers` at configure time, so a violation fails the build with
the offending edge named. If you expected a violation and did not get one, the
dependency is not declared where you think it is.
