# mono.tools/docgen - module invariants

The API reference is generated from the comments already in the headers. Nothing
in the sources carries a documentation marker and nothing is expected to.

| File | What it is |
|---|---|
| `include/`, `src/` | the filter - one function, string in, string out |
| `app/main.cpp` | the thin main Doxygen calls, one file per invocation |
| `Doxyfile.in` | every setting that is a decision, and none that are not |
| `RunDoxygen.cmake` | generates the themed header, then runs Doxygen |
| `tests/` | a Catch2 suite the test runner finds like any other |

```
just docs          build the site
just docs-serve    build it and serve it on :8000
just docs-check    fail if a public entity is undocumented
```

[`docs/CODE_DOCUMENTING.md`](../../docs/CODE_DOCUMENTING.md) is the guide for
somebody writing comments. This file is why the machinery is the shape it is.

## The filter exists so the sources do not have to change

Doxygen reads a comment written `///`, `//!`, `/**` or `/*!`. Every comment in
this repository is a plain `//`, because `docs/CODE_FORMAT.md` asks for prose
that reads as prose and a marker earns its place by doing something.

The two ways to reconcile that are to edit every header or to filter on the way
in. Filtering won because the alternative spends a marker on every comment in
the engine, forever, to satisfy a tool - and because a comment that has to be
marked is one somebody will forget to mark.

The cost is a moving part, which is why the moving part has tests.

## The line count is the invariant

**A filter that adds or removes a line is a filter that has broken every link
into the source listing.** Doxygen numbers "go to the source code of this file"
from the filtered text and links to it by line, so one extra line puts every
anchor on the page one line out - and the page still renders, still looks
right, and points at the wrong line.

This is why `@file` is written *over* a blank line that was already beside the
opening comment rather than inserted above it. Every header here is
`#pragma once`, blank, prose, so there is always one to take.

The first test in `tests/Filter.cpp` is that test. Do not weaken it.

## A trailing comment is `///<`, not `///`

`float FieldOfView = 1.22f;  // 70 degrees` documents the field to its left.
Promoted to plain `///` it would attach to the *next* member instead, so the
rendered page would put "70 degrees" against the wrong field and read as though
it belonged there.

There is exactly one trailing comment in the engine today. The rule is not there
for that one - it is there so the second one is right without anybody noticing
it was a question.

## `<word>` is escaped, because Doxygen thinks it is HTML

`<assets>/shaders/<module>/` is a path with two placeholders. Doxygen parses
both as HTML tags, warns, and renders `/shaders//`. The prose loses its meaning
and the page does not say that it did.

So the filter escapes a placeholder-shaped `<word>` on the way past. The
consequence is that **literal HTML in a comment is not supported**, which is
intended: the prose here is prose and markdown.

`<engine/core/Name.hpp>`, `std::map<K, V>` and `a < b` are not placeholder-shaped
and are left exactly as written.

## The filter is scoped to `*.hpp`, and that is load-bearing

`FILTER_PATTERNS = *.hpp=...`, never `INPUT_FILTER`. `INPUT_FILTER` runs on
every input file, and the input includes the repository's markdown - so docgen
would be handed `README.md` and apply C++ rules to it.

It did, once. The `//` in a URL became a comment marker, and the paragraph in
the root `AGENTS.md` *about* comment style was rewritten by the convention it
was describing. A filter is a language tool and the wiring has to name the
language.

## Public headers only

`src/` is the private include directory and `app/` is a thin main. Documenting
either would put a module's insides on a page headed "API reference", and the
tier system exists so that what a caller may see is decided rather than
discovered.

The repository's own markdown *is* included, and `README.md` is the front page.
An API reference that does not contain `RUNNING.md` is a second place to look.

## `RunDoxygen.cmake` is CMake, and that is the judgement call

`mono.tools/AGENTS.md` says a tool is C++ when it is a program and CMake when
its input is CMake's own output. This is neither: it is four file operations
around two process launches.

It is CMake because the rule that matters is the one behind that sentence - the
prerequisite list stays CMake, Ninja, a C++20 compiler and `glslc`. Writing it
in C++ would mean a second binary whose only job is to call the first. If it
grows past patching one `</head>`, it should become part of `docgen` instead.

## The header is generated, not checked in

`doxygen -w` stamps its own version into the header it writes. A checked-in
header warns on every run against a different Doxygen and eventually renders
against markup that has moved, so the `docs` target regenerates it each time.
A contributor on 1.9 and one on 1.14 both get a correct page.

## Doxygen is a documentation prerequisite, not a build one

A machine without it configures, builds and tests exactly as before. What it
must not do is leave `just docs` reporting "no such target" - that reads as a
broken repository rather than a missing program, so the target exists either way
and the version without Doxygen prints how to install it.

## `docs-check` is two passes, and it has to be

The root `AGENTS.md` says a rule the build does not check is documentation.
"Public headers are documented" is a rule, so `just docs-check` is the half that
checks it.

It runs Doxygen twice over one configuration, because one pass cannot do both
jobs:

- The **site** pass has `EXTRACT_ALL = YES`, so the whole public surface is on
  the page whether or not somebody documented it. A method that simply does not
  appear reads as one that does not exist. It catches malformed comments and
  dangling links.
- The **coverage** pass has `EXTRACT_ALL = NO` and generates no HTML, which is
  most of why it is nearly free. It is the only one that can see a missing
  comment.

**`EXTRACT_ALL` disables `WARN_IF_UNDOCUMENTED` and reports nothing about having
done so.** That is the trap: a single-pass configuration with both settings on
looks like it is enforcing coverage and is not. This was live for part of a day
and the log was clean the entire time, which is exactly how it survives.

`Doxyfile.check.in` is the one-line difference. Do not fold it back into
`Doxyfile.in`.

**It does not pass as of v0.1.** There are 263 gaps - 42 types and 219 members,
concentrated in `core`, `render` and `ecs`. Most are types whose prose is on the
file rather than on the type, which leaves the class page empty for a reader who
arrived from the search box. `docs/CODE_DOCUMENTING.md` explains the
distinction. That is a number to drive down, not a bar to lower.
