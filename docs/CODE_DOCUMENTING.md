# Documenting code

The API reference at `just docs` is generated from the comments already in the
headers. There is no separate documentation tree to keep in step, no manifest of
what to document, and no marker to remember.

This file is the part that is not automatic: where a comment goes, what the
tags do, and the handful of things that will bite you.

[`CODE_FORMAT.md`](CODE_FORMAT.md) covers *when* a comment is worth writing —
comment the decision, not the mechanics. That still applies. Nothing here asks
you to write more comments; it says where the ones you write end up.

Also as a standard practice, document LAST so you do not have to keep re-writing the documentation for code that will be replaced. Use a `// TODO: document` marker so you can find all the locations.

---

## Nothing to register

Doxygen only reads a comment written `///`, `//!`, `/**` or `/*!`. Every comment
in this engine is a plain `//`.

`mono.tools/docgen` reconciles that by rewriting the comments on the way into
Doxygen. The sources on disk are never touched:

```cpp
// A stable name, and a cheap handle for it.      // <- what you write
/// A stable name, and a cheap handle for it.     // <- what Doxygen is shown
```

So: **write an ordinary comment above the thing, and it is documentation.** To
see exactly what Doxygen gets for a file, run the filter yourself:

```sh
.cache/build/dev/tools/docgen mono.engine/core/include/engine/core/Name.hpp
```

Only `include/` is documented. `src/` is the private include directory and
`app/` is a thin main, and neither belongs on a page headed "API reference".

---

## Where a comment goes

| Position | Documents | Becomes |
|---|---|---|
| The block at the top of the file | the file | `/// @file` |
| Above a `class`, `struct`, `enum` | that type | `///` |
| Above a member or free function | that member | `///` |
| After code on the same line | the thing to its **left** | `///<` |

```cpp
#pragma once

// What this file is for.                  <- the file
//
// And what it deliberately is not.

#include <cstdint>

namespace engine::core {

	// What this type is for.               <- the type
	class Camera {
	  public:
		// What this does.                  <- the member
		void Focus(Vector3 target);

		float FieldOfViewRadians = 1.22f;  // 70 degrees      <- the field, to its left
	};
}
```

A trailing comment becomes `///<` rather than `///` because `///` would bind to
the *next* member — the page would put "70 degrees" against `NearPlane` and read
as though it belonged there.

---

## The file comment is not the type comment

**This is the one that catches everybody.** The file's opening prose documents
the *file*. It does not document the class inside it, even when the file holds
exactly one class and the prose is obviously about it.

`Name.hpp` opens with twenty-five lines explaining interning, stable ids and why
serializing `Id()` defeats the point. All of it lands on the *file* page. The
`engine::core::Name` class page — the one people reach from search and from the
sidebar — has an empty description.

So give the type its own comment, even a short one:

```cpp
#pragma once

// A stable name, and a cheap handle for it.
//
// [...twenty-five lines on why this type exists...]

namespace engine::core {

	// An interned string, compared as an integer. Valid within one process:
	// serialize Text(), never Id().
	class Name {
```

The file comment is the essay. The type comment is the sentence somebody needs
when they land on the class page from a search box. They are different jobs and
the second one is short.

`just docs-check` reports every type and member without one.

---

## The first sentence is the summary

`JAVADOC_AUTOBRIEF` is on, so the first sentence becomes the brief shown in
listings and the rest becomes the detail on the page. Write the first sentence
to survive on its own:

```cpp
// Interns. Cheap to repeat — the second call for the same text is a hash
// lookup — but not free, so do it once and keep the result rather than
// constructing from a literal inside a loop.
explicit Name(std::string_view text);
```

Listings show *Interns.* The page shows the whole thing.

A blank comment line separates paragraphs:

```cpp
// One sentence, which is the summary.
//
// A second paragraph, which is not.
```

`@brief` overrides the first-sentence rule when the natural opening does not
work as a summary. Reach for it rarely.

---

## Markdown

Markdown works everywhere, and it is how to format prose — not HTML.

```cpp
// Interns once and hands back a dense counter.
//
// **Bold** for the load-bearing sentence, `code` for identifiers and
// *italic* sparingly.
//
// - a list item
// - another
//
//     an indented block, rendered as code
//     second line
//
// | Column | Column |
// |---|---|
// | a | b |
```

For a runnable example prefer a fenced block, which gets a copy button and
syntax highlighting:

```cpp
// @code
// Name transform("Transform");   // once, at load
// if (a == b) { ... }            // integer compare, forever after
// @endcode
```

---

## Tags

Everything below is verified against this repository's configuration.

### The ones you will actually use

| Tag | For |
|---|---|
| `@param name` | one argument |
| `@return` | the return value |
| `@throws Type` | an exception that escapes |
| `@brief` | an explicit summary, overriding the first sentence |
| `@note` | an aside worth a callout box |
| `@warning` | the thing that will cost somebody an afternoon |
| `@see Other` | a pointer to the related thing |
| `@since v0.2` | the version it arrived in — the versions in `ROADMAP.md` |
| `@deprecated` | say what to use instead, in the same sentence |
| `@code` / `@endcode` | a fenced, highlighted, copyable example |
| `@ref Thing` | an explicit link where autolinking will not fire |

```cpp
// Pins `text` to exactly `id`, for a number that is itself part of a format.
//
// @param text The name to intern.
// @param id   The number the format already assigns to it.
// @return An invalid Name if either the id or the text is already taken,
//         which is a startup error worth failing on.
// @warning Prefer not to need this. An id that is not first-seen order is a
//          number somebody has to keep true by hand.
// @see Name::FromId
static Name Reserve(std::string_view text, uint32_t id);
```

**`@param` is all or nothing.** Documenting one argument and not the rest is a
warning, and `just docs-check` fails on it. Documenting none is fine — a
parameter whose name and type say everything does not need a line repeating
them. This is deliberate: half-documented is worse than undocumented, because it
reads as complete.

### The engine's own

These are the equivalent of Moonwave's `@within` and `@server`. Each collects
into its own index page, so "what is client-only?" is a page rather than a grep.

| Tag | Index page | Put it on |
|---|---|---|
| `@tier` | Tiers | the file comment |
| `@client` | Client-only API | anything a server build does not link |
| `@server` | Server-only API | anything a client build does not link |
| `@threadsafe` | Thread-safe API | a type or function safe to call from several threads |
| `@tick` | Tick-bound API | work that must finish inside the tick that started it |

`@tier` takes free text, so write it the way the module reads:

```cpp
#pragma once

// The RHI, such as it is at v0.1.
//
// @tier L12 · client
```

This replaces the three spellings currently in the tree — `L3 ·`, `L12
[client] ·`, `L1.` — with one that is also a link. The tier itself is enforced
by `mono_check_all_tiers`; the tag is how it reaches the page.

`@client` and `@server` carry their own sentence already, so they take no
argument:

```cpp
// Uploads the frame's instance buffer.
//
// @client
void Upload(const Frame &frame);
```

---

## Cross-references

Doxygen autolinks anything it recognises. All three of these become links with
no markup at all:

```cpp
// Returns a Name, or engine::core::Name if you prefer it spelled out.
// Call Reserve() first when the id is part of a format.
```

`Name`, `engine::core::Name` and `Reserve()` all link. Use `@ref` only when
autolinking will not fire — usually a markdown page linking into the API.

Linking to another document is an ordinary markdown link, and it works both on
GitHub and on the generated site:

```markdown
See [RUNNING.md](RUNNING.md) and [the tier rule](#the-layer-stack-is-not-negotiable).
```

---

## Things the filter does for you

**Angle-bracket placeholders are escaped.** `<assets>/shaders/<module>/` is a
path with two placeholders in it, and Doxygen would read both as HTML tags,
warn, and render `/shaders//` — losing the meaning without saying it had. Write
the path normally:

```cpp
// <assets>/shaders/<module>/. A module stages its own SPIR-V under its own
// name so that two modules cannot collide on fullscreen.vert.
static std::filesystem::path Shaders(std::string_view module);
```

`<engine/core/Name.hpp>`, `std::map<K, V>` and `a < b` are not placeholder-shaped
and are left exactly as written.

**The consequence is that literal HTML in a comment is not supported.** That is
intended. Use markdown.

**A comment marker you write on purpose is left alone.** `///` and `//!` pass
through untouched, so a header that wants to be explicit can be.

---

## Things that will bite you

**A type with no comment of its own gets an empty page.** The section above; it
is the most common gap in the engine today.

**A `@param` that names an argument that is not there is an error, not a
no-op.** Rename an argument and the comment fails the check rather than quietly
documenting nothing.

**Documenting some arguments and not others fails.** All or nothing.

**A comment inside a function body is not documentation.** Doxygen does not read
function bodies. That is fine — those comments are for the reader of the code,
which is the point of most of them.

**`src/` is invisible.** A comment on a private header documents nothing on the
site. It is still worth writing; it is just not reference material.

**Do not hand-edit anything under `.cache/build/*/docs/`.** It is regenerated,
and `just clean` deletes it.

---

## A complete example

```cpp
#pragma once

// One clock, many tick rates.
//
// The engine runs several loops at different rates against the same wall
// clock, and every one of them has to agree on what "now" is — otherwise a
// recorded run does not replay.
//
// @tier L1 · shared

#include <cstdint>

namespace engine::core {

	// A fixed-rate tick derived from one wall clock.
	//
	// Construct one per rate and step it from the frame loop. The accumulator
	// is inside, so a caller never sees a partial tick.
	class Clock {
	  public:
		// Ticks per second. Chosen rather than derived: 60 is what the
		// physics was tuned against.
		static constexpr int DEFAULT_RATE = 60;

		// @param ticksPerSecond How often Step should report a tick.
		explicit Clock(int ticksPerSecond = DEFAULT_RATE);

		// Advances by one frame and reports how many ticks are owed.
		//
		// The count can be zero on a fast frame and more than one after a
		// stall, which is why it is a count rather than a bool.
		//
		// @param deltaSeconds Wall time since the previous call.
		// @return Ticks to run before drawing.
		// @tick
		int Step(double deltaSeconds);

		// Seconds per tick. Constant for the life of the clock.
		double Interval() const;

		double Accumulated = 0.0;  // Unspent time, in seconds.
	};
}
```

---

## What the build checks

```sh
just docs          # build the site
just docs-serve    # build it and serve it on http://localhost:8000
just docs-check    # fail on gaps and on malformed comments
```

`docs-check` is two Doxygen passes, because one cannot do both jobs:

- The **site** pass lists the whole public surface, documented or not. An
  undocumented method that simply does not appear reads as one that does not
  exist. It reports malformed comments and dangling links.
- The **coverage** pass generates no HTML and reports every public entity
  without a comment. It has to be separate: `EXTRACT_ALL`, which the site pass
  needs, switches off the undocumented warning and reports nothing about having
  done so.

Both must be clean. This is `AGENTS.md` rule 6 applied to documentation — a rule
the build does not check is documentation, and "public headers are documented"
is a rule.

**It does not pass yet.** As of v0.1 there are 263 gaps: 42 types and 219
members, concentrated in `core` (114), `render` (62) and `ecs` (27). Most are
types whose prose is on the file rather than on the type. That is a number to
drive down, not a bar to lower.

`mono.tools/docgen/AGENTS.md` has the reasoning behind the filter itself,
including why the line count is an invariant and why it is scoped to `*.hpp`.
