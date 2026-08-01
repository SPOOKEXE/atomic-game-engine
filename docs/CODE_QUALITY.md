# Code quality checklist

**The completion checklist.** Work through this before opening a pull request,
and again when reviewing one. `CONTRIBUTING.md` points here rather than keeping
a second copy — one checklist, in one place, or the two drift and nobody knows
which is current.

Written as questions rather than rules, because the useful version of most of
these is "did you think about it", not "did you comply".

A reviewer should be able to work through this in a few minutes. If a change is
too large for that, it is too large.

Working with Claude Code, `/run-checklist` walks the whole thing and reports
what it could not verify rather than skipping it quietly.

---

## 1 · The mechanical gate

None of this needs judgement, so none of it is worth a reviewer's attention.
Get it green first.

- [ ] **`just format`** — and check whether it changed anything. A
      formatting-only diff mixed into a behavioural change is what makes a
      review hard to read.

      It exits 1 if `clang-format` is not installed rather than pretending to
      have run. If you do not have it, say so in the pull request; do not
      hand-format to match. `just format-check` is the non-mutating version.
- [ ] **Both presets configure and build.** `dev` builds everything; `server`
      builds with no client at all. A `client`-tier dependency that a `shared`
      module picked up by accident only fails in the second one.
- [ ] **Every test passes, in both.** `ctest`, not `just test` — the cache is
      an optimisation for the inner loop, not something a pull request should
      rely on.
- [ ] **`just check-server-is-headless`** — fails if the staged `server/`
      directory has grown a `shaders/` folder.
- [ ] **The `ci` preset builds.** That is `dev` with warnings fatal.

```sh
just format
cmake --preset dev    && cmake --build .cache/build/dev -j
cmake --preset server && cmake --build .cache/build/server -j
( cd .cache/build/dev    && ctest --output-on-failure )
( cd .cache/build/server && ctest --output-on-failure )
just check-server-is-headless
cmake --preset ci && cmake --build .cache/build/ci -j
```

- [ ] **Did you read the `AGENTS.md` of every folder you touched?** The root one
      carries policy; the per-folder ones carry the invariants that actually
      catch mistakes. This is the step most often skipped and the one that most
      often would have helped.

---

## 2 · Architecture

- [ ] **Does every new dependency edge go downward?** The tier check catches
      client/server mistakes; it does not catch L3 including L7. Check the
      layer heights by hand.
- [ ] **Is a new module recorded in
      `mono.tools/architecture/expected_graph.json`?** The architecture test
      fails otherwise, and that failure is the point.
- [ ] **Does anything new cross a world boundary as a pointer?** It must be a
      copy, and the copy must be describable as a schema.
- [ ] **Is there now a second way to do something that already existed?** Two
      ways is the expensive kind of debt, because both grow callers.
- [ ] **Is a new public header actually public?** If only this module uses it,
      it belongs in `src/`.
- [ ] **Did a vendor type reach a public header?** `VENDOR_PUBLIC` is a
      deliberate widening, not a convenience.
- [ ] **Is anything identified by a number that leaves the process?** A
      component id, an enum value, an asset key. If it reaches a file, a wire
      format or a manifest, the string is the contract — `core::Name` interns
      it, and `Id()` is session-local.
- [ ] **Is a new leaf library in `mono.engine/` when it depends on nothing?**
      Only once there are three of them; one leaf is a module.

## 3 · Correctness

- [ ] **What happens on the empty input?** Zero entities, an empty span, a
      minimised window with no pixels, a store with nothing in it.
- [ ] **What happens on the first frame?** Delta is zero, there is no previous
      frame, nothing has been measured yet. A surprising number of bugs live
      here and only appear at startup.
- [ ] **Is a division guarded?** Frame time, magnitude, aspect ratio. A zero
      here becomes a NaN three subsystems away, where it is unrecognisable.
- [ ] **Is every allocation's growth bounded?** A vector that is `push_back`ed
      per frame and never cleared is invisible for the first minute.
- [ ] **Does an error path leave the object usable, or at least obviously
      dead?** Half-initialised is worse than either.
- [ ] **Is a resource released on every path out, including the early
      returns?** RAII where possible; if not, say why in a comment.

## 4 · Concurrency

- [ ] **Which thread is this on?** If the answer is "whichever", say so and
      make it safe. If it is "the one that owns the store", let the affinity
      check say so.
- [ ] **Can an exception escape into a thread that did not throw it?** It
      terminates the process. `Jobs::For` catches and rethrows on the caller
      for exactly this reason.
- [ ] **Is a lock held across a call into another subsystem?** That is how a
      deadlock gets built out of two reasonable pieces of code.

## 5 · Performance

- [ ] **Have you profiled it, or only reasoned about it?** F5 for a frame
      breakdown with nothing attached, Tracy for the real thing. "Should be
      fast" is not a finding, and neither is "it feels the same".
- [ ] **Did you profile the right build?** `dev` is `-O0` on purpose, so it
      shows what the code does; `release` shows what ships. The gap is over an
      order of magnitude. Say which one a number came from.
- [ ] **Does it allocate per frame?** Per entity per frame?
- [ ] **Is the cost proportional to what changed, or to how much exists?** A
      pass over every entity to find the three that moved is the shape to
      notice.
- [ ] **Is instrumentation itself in the measurement?** A reserve, a
      reallocation or a log line inside the timed region distorts what it
      reports. A diagnostic helper called from a system counts — `CountMatching`
      once built a query per call and was the most expensive thing in the
      server's tick. It caches its query now, but the shape recurs: a helper
      that is cheap to call once is not automatically cheap to call per tick.
- [ ] **If it went parallel, was it compared against serial?** Below a crossover
      `EachParallel` is slower than `Each`, and for a cheap body that crossover
      is tens of thousands of rows. A/B it in `release` and record both numbers.
- [ ] **Was the grain chosen or inherited?** `DEFAULT_GRAIN` suits a body that
      does almost nothing. Real work per row wants a much smaller one.

## 6 · Tests

- [ ] **Does the test fail if the code is wrong?** Write it, break the code
      deliberately, watch it go red. A test that passes both ways is worse than
      none, because it is believed.
- [ ] **Does it test behaviour or implementation?** A test that has to change
      whenever the code is refactored is a cost with no benefit.
- [ ] **Is the interesting case covered, or only the easy one?** Empty, one,
      many, wrong.
- [ ] **Is every code path you added reachable from a test?** Not line
      coverage as a number — the branches. An error path with no test is an
      error path that has never run.
- [ ] **Does the file have a `TEST_SUITE_ID`?** The runner cannot see it
      otherwise, and it will be silently skipped.
- [ ] **Are `TEST_DEPENDS` declared?** They are what makes the cascade
      re-run this suite when something under it changes.
- [ ] **Does it need a GPU, a network or a clock?** If so, can the part that
      does not be separated out so CI still covers it?

## 7 · Security

The two untrusted boundaries are: a server loading a game file from its
operator, and a client loading one from whatever server it connected to. Both
are hostile.

- [ ] **Is a length or an index from outside bounded before it is used?**
- [ ] **Is parsing separated from building?** Validate to a checked
      description first; construct from the description second. Fusing the two
      is the standard way this class of parser gets exploited.
- [ ] **Does a new parser have a fuzz target?**
- [ ] **Does an error message leak a path, a token or an address?**

## 8 · Craft

The questions that have no build step behind them. They are last because they
are the easiest to skip and the most expensive to leave, and a reviewer will ask
them whether or not you did.

- [ ] **Can you improve it further?** Not "is it finished" — is there a version
      of this you would rather maintain. Ask it once, honestly, before you stop.
      The answer is often no, and asking still costs a minute.
- [ ] **Is anything named generically?** `data`, `info`, `manager`, `handler`,
      `value`, `temp`, `result`, `process`, `util`. A name that would fit
      anywhere describes nothing. `deltaSeconds`, not `dt`; `instanceCount`, not
      `n`; and units in the name where there are units — `FrameMilliseconds`,
      `StartNanoseconds`. A number whose unit you have to look up will
      eventually be added to one in a different unit.
- [ ] **Is there dead code to remove?** An unreachable branch, a parameter
      nobody passes, a function with no callers, a commented-out block, an
      option that does nothing. Delete it — git has it. Dead code is read as
      live by everybody who comes after you, including the next model.
- [ ] **Should this file be more than one file?** If you cannot say what a file
      is *for* in one sentence, it is doing two jobs. The same applies to a
      function: if half of it is policy and half is byte-shuffling, the
      byte-shuffling wants a name.
- [ ] **Is there a second way to do something that already existed?** Delete
      the one you replaced. Two ways to do one job is the most expensive kind of
      debt in a monorepo, because both accumulate callers.
- [ ] **Is the math and algorithms correct?** Fix these issues immediately
      and ensure tests cover them.

### Negative C++ practices

Specific, and all of them appear in code that compiles and passes:

- [ ] `new`/`delete` where a value, a `unique_ptr` or a container works.
- [ ] Owning raw pointers. A raw pointer is a non-owning observer; anything
      else says so in the type.
- [ ] A reference or `string_view` outliving what it points at — returning one
      to a local, or storing one whose owner is a temporary. `FrameSpan::Name`
      is a `string_view` and only ever a literal *because* of this.
- [ ] Passing a large type by value in a hot path, or by `const&` when it is
      two words and copying is cheaper.
- [ ] `catch (...)` with no comment saying what it is protecting against.
- [ ] Signed/unsigned comparison, narrowing without a cast, or `int` where the
      value is an index into something that could exceed 2^31.
- [ ] A `static` non-trivial global. Initialisation order across translation
      units is unspecified — use a function-local static, which is also
      thread-safe on first use.
- [ ] A `#include` in a public header that only the `.cpp` needs. Forward
      declare instead; `Renderer.hpp` names `struct SDL_Window;` for this
      reason.
- [ ] A macro where a `constexpr`, an `inline` function or a template works.
      The profiling macros exist because they need `__LINE__` and a scope; that
      is the bar.
- [ ] Undefined behaviour you are relying on because it happens to work — signed
      overflow, strict aliasing, reading an uninitialised value, out-of-bounds
      by one. "It works on this compiler" means the optimiser has not yet
      noticed.

### Negative general practices

- [ ] A magic number with no name and no comment saying where it came from.
- [ ] An error swallowed — a return value ignored, a failure logged and then
      continued past as though it had not happened.
- [ ] A `TODO` with no version or owner. `TODO(v0.5):` is a plan; `TODO:` is a
      wish.
- [ ] A comment that restates the line instead of explaining the decision.
- [ ] Copy-paste with one thing changed, where the difference should have been
      a parameter — or worse, where it should not have been copied at all.
- [ ] A test written after the fact that passes whether or not the code is
      right.

## 9 · Documentation

- [ ] **Does the module's `AGENTS.md` still tell the truth?** If a change
      invalidates an invariant written there, updating it is part of the change.
- [ ] **Is a non-obvious decision explained where it was made?**
- [ ] **Does a new command-line flag appear in `--help`?** It does
      automatically if declared through `Arguments`, which is why flags are
      declared rather than scanned for.
- [ ] **Is a `TODO` versioned?** `TODO(v0.5):` is a plan.

---

## 10 · Reporting

- [ ] **Does the pull request say what you did not do?** A change that names
      its own gaps is easier to trust than one that mentions none.
- [ ] **Are the numbers attributed?** A frame time is meaningless without the
      preset it came from — `dev` builds first-party code at `-O0` on purpose,
      and the gap to `release` is over an order of magnitude.
- [ ] **Is anything reported as passing that was not actually run?** If a step
      could not run — no GPU, a missing tool — say that. A confident wrong
      report is the most expensive thing this checklist exists to prevent.

---

## The one that matters most

**Would you be able to explain this change, out loud, to somebody who has not
read it?** If the honest answer is no — because it was generated, or copied, or
arrived at by trying things until the tests passed — it is not ready, whatever
the tests say.
