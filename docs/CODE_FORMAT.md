# Code format

`.clang-format` is the authority for anything mechanical. Run `just format`, or
let your editor do it. What follows is the part a formatter cannot decide.

Nothing here is a matter of taste worth arguing about. It is written down so
that nobody has to argue about it.

---

## The mechanical part

Tabs, width 4. 110 columns. Braces on the same line. Pointers bind right
(`Type *name`). Namespaces indent their contents.

Tabs rather than spaces so that a contributor who wants a different indent
width can have one without touching the file.

---

## Naming

| Thing | Case | Example |
|---|---|---|
| Namespace | lower | `engine::core` |
| Type | Pascal | `FrameGraph`, `OverlayImage` |
| Function and method | Pascal | `BeginFrame`, `PointToWorldSpace` |
| Public data member | Pascal | `Position`, `FrameMilliseconds` |
| Local and parameter | camel | `deltaSeconds`, `instanceCount` |
| Constant and macro | SCREAMING | `MAXIMUM_SPANS`, `ENGINE_PROFILE` |
| File | Pascal, matching its main type | `FrameGraph.hpp` |

Pascal members rather than `m_` or a trailing underscore, because this engine
is for Roblox developers and the scripting surface is Pascal on both sides of
the binding. One convention across C++ and Luau is worth more than the extra
information a prefix carries.

**Spell the word out.** `deltaSeconds`, not `dt`. `instanceCount`, not `n`. The
exception is a loop index in a three-line loop, and even there `index` costs
nothing.

**Units belong in the name.** `FrameMilliseconds`, `RadiansPerSecond`,
`StartNanoseconds`. A number whose unit you have to look up is a number that
will eventually be added to one in a different unit.

---

## Includes

Grouped, and `.clang-format` regroups them:

1. This file's own header, if it is a `.cpp`.
2. `<engine/...>` — first-party.
3. Vendor — `<SDL3/...>`, `<glm/...>`, `<spdlog/...>`.
4. The standard library.

Angle brackets for anything reachable through an include directory, quotes only
for a private header in the same module's `src/`. That way the form of the
include says which side of the module boundary the file is on.

---

## Headers

**Public headers go in `include/engine/<module>/` and nowhere else.** A header
in `src/` is unreachable from another module, and CMake enforces it. Put a
thing in `src/` unless another module needs it.

`#pragma once`. Include what you use. Forward-declare where you can —
`Renderer.hpp` declares `struct SDL_Window;` rather than including SDL, and
that one line is why `Instance` and `Camera` are usable without a graphics API.

---

## Comments

**Comment the decision, not the mechanics.** The code says what it does. A
comment earns its place by saying something the code cannot:

```cpp
// Cycling hands back a fresh allocation rather than stalling on the copy the
// previous frame may still be reading.
SDL_UploadToGPUBuffer(copy, &source, &destination, true);
```

Good reasons to write one:

- The obvious thing is wrong here, and this explains why.
- There is a constraint from outside this file — an API's convention, a
  platform's behaviour, a format's layout.
- A future reader will want to "simplify" this and break it.
- A number was chosen rather than derived, and this is where it came from.

Bad reasons: restating the line, narrating the steps, or marking sections of a
function that should have been two functions.

**A file header says what the file is for and what it deliberately is not.**
The first twenty lines of `FrameGraph.hpp` explain why it exists next to Tracy;
that is the question a reader arrives with.

**British or American spelling — pick one per file and do not churn.** The
codebase leans British (`Colour` in local code, `Color3` where it mirrors the
Roblox type). Matching the surrounding file matters more than either.

---

## Functions

**Early return over nesting.** Guard clauses first, then the body at one level
of indentation.

**One level of abstraction per function.** If half of it is policy and half is
byte-shuffling, the byte-shuffling wants a name.

**No output parameters where a return value works.** A struct return is free.

---

## What the formatter cannot catch, and reviewers should

- A `TODO` without a version or an owner. `TODO(v0.5):` is a plan. `TODO:` is
  a wish.
- A magic number with no name and no comment.
- A `catch (...)` that does not say what it is protecting against.
- A commented-out block. Delete it; git has it.
- Two spellings of the same concept in one module.
