# Documenting code

The API reference at `just docs` is generated from the comments already in the
headers. There is no separate documentation tree to keep in step, no manifest of
what to document, and no marker to remember.

This file is the part that is not automatic: where a comment goes, what the
tags do, and the handful of things that will bite you.

[`CODE_FORMAT.md`](CODE_FORMAT.md) covers *when* a comment is worth writing -
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
the *next* member - the page would put "70 degrees" against `NearPlane` and read
as though it belonged there.

---

## The file comment is not the type comment

**This is the one that catches everybody.** The file's opening prose documents
the *file*. It does not document the class inside it, even when the file holds
exactly one class and the prose is obviously about it.

`Name.hpp` opens with twenty-five lines explaining interning, stable ids and why
serializing `Id()` defeats the point. All of it lands on the *file* page. The
`engine::core::Name` class page - the one people reach from search and from the
sidebar - has an empty description.

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
// Interns. Cheap to repeat - the second call for the same text is a hash
// lookup - but not free, so do it once and keep the result rather than
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

Markdown works everywhere, and it is how to format prose - not HTML.

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
| `@since v0.2` | the version it arrived in - the versions in `ROADMAP.md` |
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
warning, and `just docs-check` fails on it. Documenting none is fine - a
parameter whose name and type say everything does not need a line repeating
them. This is deliberate: half-documented is worse than undocumented, because it
reads as complete.

**`@param` takes a direction**, and it earns its keep on the arguments where the
signature does not already say. A `const &` says "in" by itself; an out
parameter and an in-out parameter look identical at the call site.

```cpp
// @param[in]     source Read, never written.
// @param[out]    result Written, never read. Its prior value is not used.
// @param[in,out] buffer Read, then overwritten in place.
```

### The rest of what works here

Less common, and all of them checked against this configuration rather than
assumed - the probe that produced these rows is in the note at the end.

| Tag | For |
|---|---|
| `@details` | an explicit detail block, when the opening paragraph should not be the brief |
| `@par Title` | a titled paragraph part-way through a long comment |
| `@exception Type` | a synonym of `@throws`. Pick one and stay with it; this repository writes `@throws` |
| `@todo` | an outstanding job. Collects into a **Todo List** page |
| `@bug` | a known defect. Collects into a **Bug List** page |
| `@anchor name` | a link target, for a `@ref name` that has nothing to autolink to |
| `@defgroup id Title` | opens a group, with `@{` and `@}` around its members |
| `@ingroup id` | puts one entity into an existing group |
| `@addtogroup id` | reopens a group to add more to it |

`@todo` and `@bug` are worth knowing apart from the `// TODO(v0.5): ...` markers
already in the tree. Those are notes to whoever opens the file; these are index
pages somebody can read without opening anything. A deferred *decision* belongs
in [`DEFERRED.md`](DEFERRED.md) - `@todo` is for the small outstanding job that
does not need an entry there.

Grouping is the one to reach for deliberately rather than often. The site is
already organised by module and by tier, so a group earns its place only when it
cuts across both:

```cpp
// @defgroup coordinates Coordinate conversion
// @brief Every function that moves a value between two spaces.
// @{

Vector3 WorldToLocal(const CFrame &frame, Vector3 point);
Vector3 LocalToWorld(const CFrame &frame, Vector3 point);

// @}
```

### Tags this repository does not use

Not because they are broken - they work - but because something else here
already does the job, and two records of one fact disagree eventually.

| Tag | Instead |
|---|---|
| `@author`, `@date` | `git log` and `git blame`, which cannot go stale |
| `@copyright`, `@license` | `LICENSE`, once, for the whole repository |
| `@class`, `@struct`, `@fn` | nothing - the comment already sits above the thing it documents, and these only exist for comments that do not |
| `@mainpage` | `USE_MDFILE_AS_MAINPAGE`, which makes README.md the front page |
| `@page` | a markdown file under `docs/`. Every one already becomes a page |
| `@tableofcontents` | nothing. It only has an effect inside a `@page` or `@mainpage` body, so it does nothing in a header comment |

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

This replaces the three spellings currently in the tree - `L3 ·`,
`L12 [client] ·`, `L1.` - with one that is also a link. The tier itself is enforced
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
autolinking will not fire - usually a markdown page linking into the API.

Linking to another document is an ordinary markdown link, and it works both on
GitHub and on the generated site:

```markdown
See [RUNNING.md](../RUNNING.md) and [the tier rule](#the-layer-stack-is-not-negotiable).
```

---

## Things the filter does for you

**Angle-bracket placeholders are escaped.** `<assets>/shaders/<module>/` is a
path with two placeholders in it, and Doxygen would read both as HTML tags,
warn, and render `/shaders//` - losing the meaning without saying it had. Write
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

**One comment documents one declaration - the next one.** A block written above
two related lines documents the first and leaves the second bare, which is easy
to miss precisely because the pair reads as one idea:

```cpp
// Moves the accumulated state.
Sha256(Sha256 &&) noexcept;
Sha256 &operator=(Sha256 &&) noexcept;   // <- undocumented, and reads as covered
```

Give each its own line of prose, however short. A move constructor and a move
assignment do not say the same thing anyway: one leaves a source behind, the
other also discards a destination.

**A `@param` that names an argument that is not there is an error, not a
no-op.** Rename an argument and the comment fails the check rather than quietly
documenting nothing.

**Documenting some arguments and not others fails.** All or nothing.

**A comment inside a function body is not documentation.** Doxygen does not read
function bodies. That is fine - those comments are for the reader of the code,
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
// clock, and every one of them has to agree on what "now" is - otherwise a
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

Both must be clean. This is `AGENTS.md` rule 6 applied to documentation - a rule
the build does not check is documentation, and "public headers are documented"
is a rule.

**It passes.** `just docs-check` is clean at v0.1: every public entity in every
public header carries a comment, and no comment in the tree is malformed.

It was 263 gaps when this file was first written, and the last 56 of them were
the two programs and the test runner - `Demo.hpp` (14), `Server.hpp` (10),
`Client.hpp` (9), `Runner.hpp` (8), `Sha256.hpp` (7), `Simulation.hpp` (5) and
`Process.hpp` (3). Almost all were fields: a struct whose *type* had prose and
whose members had none, which is the shape the section above warns about.

Keep it at zero. A check that has been failing for a while stops being read, and
takes the real failures down with it - which is exactly what happened here. The
site pass was failing on a warning that looked like it came from README.md, and
because the site pass runs first, the coverage pass behind it had never run at
all. Nobody had seen the gap count because nothing had ever printed one.

The warning turned out to have nothing to do with README.md. Four empty
`docs/index.md` placeholders each became a page, each was titled from its
filename for want of a heading, and each claimed the page label `index` that the
main page already holds. **An empty markdown file is not an inert one.**
`mono.tools/docgen/CMakeLists.txt` now leaves pages with no content out of the
site, and says so at configure time rather than dropping them quietly.

`mono.tools/docgen/AGENTS.md` has the reasoning behind the filter itself,
including why the line count is an invariant and why it is scoped to `*.hpp`.

---

## Checking a tag yourself

Doxygen has several hundred commands and this file lists the ones that are
useful here. Before adding a row to those tables, run the tag rather than
trusting a web page about a different project's configuration - `ALIASES`,
`JAVADOC_AUTOBRIEF` and the filter all change what a comment means.

Two commands answer it. The first shows what the filter hands Doxygen:

```sh
just build docgen
.cache/build/dev/tools/docgen path/to/Header.hpp
```

The second runs the real configuration over a scratch header and reports what
Doxygen made of it. Point `INPUT` somewhere outside the repository so the probe
never reaches the site, and turn the main page off with it - README.md is no
longer in `INPUT` at that point, and a main page naming a file Doxygen was not
given is a warning of its own:

```sh
just docs                                    # once, so Doxyfile exists
mkdir -p /tmp/probe && $EDITOR /tmp/probe/Probe.hpp
{ cat .cache/build/dev/docs/Doxyfile
  echo "INPUT                  = /tmp/probe"
  echo "USE_MDFILE_AS_MAINPAGE ="
  echo "OUTPUT_DIRECTORY       = /tmp/probe/out"
  echo "WARN_LOGFILE           = /tmp/probe/out/warnings.txt"
} > /tmp/probe/Doxyfile
doxygen /tmp/probe/Doxyfile && cat /tmp/probe/out/warnings.txt
```

An empty `warnings.txt` means the tag parsed. That is not the same as the tag
doing something, so check for the page it should have produced -
`/tmp/probe/out/html/todo.html` for `@todo`, `group__*.html` for a group. A tag
Doxygen does not recognise is silently rendered as text, which is the failure
mode worth catching: it looks fine in the source and reads as prose on the page.
