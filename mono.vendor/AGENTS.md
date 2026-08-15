# mono.vendor — module invariants

Third-party dependencies, shared by every `mono.X` folder that needs one.
There is no per-folder vendor directory and there should never be.

## Submodules only. No first-party code

Not one file. Vendoring a third-party source as a copy works once and then
drifts invisibly — the copy gets a local fix, upstream gets a different one,
and nobody finds out until a version bump three years later.

If a dependency needs a patch, the patch goes upstream or into a fork whose
remote is recorded in `.gitmodules`. It does not go into a file in this tree.

**Or, once, into `mono.vendor/patches/`.** That is a third shape and it is
narrow: a `.patch` applied to a *copy* of a submodule by the recipe that builds
it, never a source file sitting in this tree pretending to be upstream's. The
rule above is about *drift* — a copy that silently stops matching what it was
copied from — and a patch has the opposite property, because `git apply` fails
loudly the moment upstream moves the code underneath it.

**The contract is a directory name, and the submodule is never written to.**
`mono.vendor/patches/<name>/` holding at least one `.patch` is what makes
`mono.vendor/<name>` a patched vendor. `scripts/vendor-tree.sh <name>` is what
acts on that: it archives the pinned commit into `.cache/vendor/<name>`, applies
the patches there, and prints the path to build from. There is no list to keep
in sync — the patches say which submodule they belong to by where they live.

- **The submodule stays pristine, and the script checks it.** Patching in place
  worked and left `mono.vendor/luau-lsp` modified in every `git status` from
  then on, which is noise a reviewer learns to skip past and a submodule pointer
  one `git commit -a` away from moving by accident. Local edits there now stop
  the build with the two commands that turn them into a patch, because the build
  reads the commit rather than the files and would otherwise ignore them
  silently.
- **The copy is `git archive`, not `cp`.** The tree is the pinned commit by
  construction, so a patch always applies against upstream's preimage and a
  half-patched tree cannot exist. It also means no reverse-apply check: re-runs
  are decided by a stamp of the pinned commits and the patch contents, and a
  match skips the extraction entirely so mtimes hold still and the build is a
  no-op.

- **`luau-lsp/dotted-enum-types.patch` is the one.** It teaches the language
  server to register `importedTypeBindings["Enum"]`, so `local face:
  Enum.NormalId` resolves in an editor the way it already does in
  `just typecheck`. `docs/retired/DEFERRED.md` D00031 is the whole argument;
  what matters here is why it is a file rather than a fork.
- **A fork was the obvious answer and is worse for this.** The change keys on the
  `Enum_` prefix this repository's own generator emits, so there is nothing to
  send upstream and the fork would exist forever, tracking a moving `main` with
  one hunk on top. It also needs a remote nobody here owns and a push to it,
  where the patch needs neither — it is visible in `git diff`, reviewable as
  text, and reproducible from a fresh clone with one command.
- **Three obligations come with taking this shape, and they are what make it
  safe.** The patch carries a preamble saying what it changes and where to
  re-point it; `just luau-lsp` applies it and *stops* rather than building
  without it; and `just check` runs `just typecheck-editor`, which fails if the
  spelling the patch exists for stops resolving.

**It still does not generalise to a library the engine links, and the reason
changed.** It used to be that a patch on a vendor `just setup` clones and
anything else re-checks-out would be applied by one thing and reverted by
another; building from a copy removes that hazard, and the script is not
luau-lsp-specific. What remains is the consumer: luau-lsp is a developer tool
built in a tree of its own by one recipe, which is one place to take a source
path from. A vendor compiled by `mono.build` gets its path from
`MonoVendor.cmake`, which names `mono.vendor/<name>` directly and is read by
every preset — so patching one means teaching CMake to ask the script where the
tree is, before configure, for every consumer. That is a real change rather than
an impossible one. Until somebody makes it, the rule above is unchanged.

## One vendor, many consumers

`mono.vendor/sdl` is *the* SDL for this repository. `mono.engine/render`,
`mono.engine/input` and `mono.client` all link the same target from the same
source tree.

That is what makes the tier check meaningful: if each folder had its own copy,
"the server does not link SDL" would be a statement about one directory rather
than about the binary.

## Who may link what is a tier question, not a location question

Being in `mono.vendor/` says nothing about who may use a library. `SDL3::SDL3`
is available to any target that asks, and the reason `mono.server` has no SDL
in it is that no `server`-tier target lists it — not that it is kept somewhere
else.

Two rules follow, both enforced in `mono_add_library`:

- **`VENDOR` is private.** The library is linked, and nothing about it leaves
  the module.

  Private in the *compilation* sense, which is the one that matters here and is
  narrower than it sounds. A static archive does not absorb its dependencies, so
  CMake still puts a privately-linked vendor on the link line of everything that
  links the module — measured in `docs/CPP_LINKER.md` §4, where `libcryptopp.a`
  reaches `client` while cryptopp's headers do not. Prefer `VENDOR` because it
  keeps vendor types out of our headers and our build times down, not because it
  keeps code out of a binary. That is the linker's job, and §1 has the numbers.
- **`VENDOR_PUBLIC` is a deliberate widening,** for the cases where a vendor
  type genuinely appears in a public header — `spdlog` in `core/Log.hpp`, glm
  in `core/types/CFrame.hpp`. Every use of it
  enlarges what the rest of the engine can see, so each one should be
  defensible in review.

Prefer hiding the dependency. `mono.engine/render` links SDL privately and
forward-declares `struct SDL_Window;`, which is why `Instance` and `Camera` are
usable without a graphics API in scope.

## Some of them ship no CMakeLists, and we declare the target

imgui and asio have no usable `CMakeLists.txt` — imgui deliberately, because you
compile the two backends that match your platform rather than all of them. For
those, `MonoVendor.cmake` declares the target itself:

- **asio** is an `INTERFACE` library: an include directory plus
  `ASIO_STANDALONE`. Without that define it goes looking for Boost, which is the
  only difference between the two distributions and the whole reason this is not
  a bare `target_include_directories`.
- **imgui** is a `STATIC` library over the four core sources and the SDL3 pair,
  `EXCLUDE_FROM_ALL` so it costs nothing until something links it.

Two rules for a hand-declared vendor target:

- **`SYSTEM` on the include directories.** The `ci` preset builds first-party
  code with `-Werror`, and a warning in a vendored header must never be able to
  fail our build.
- **Declare it here, not in the module that uses it.** A target defined inside
  `mono.engine/ui/` is a target `mono.studio/` cannot link without depending on
  `ui`.

## Configuration lives in one place

Vendor options are set in `mono.build/MonoVendor.cmake`, not scattered through
the modules that consume them. A module setting `SDL_SHARED` would be a setting
that depends on directory traversal order.

## Adding one

1. `git submodule add --depth 1 -b <stable branch> <url> mono.vendor/<name>`
   — pin a release branch, not a default one that moves.
2. Configure it in `mono.build/MonoVendor.cmake`, with `EXCLUDE_FROM_ALL` if it
   has unconditional install rules.
3. Add a line to `THIRD_PARTY_NOTICES.md`.
4. Check the licence is compatible with MPL-2.0 before any of the above.

A shallow clone plus a superproject-pinned SHA is fast *and* reproducible. Both
matter: `just setup` is the first thing a new contributor runs.

## Not adding one, three times over one decision, and each was argued rather than assumed

A submodule is the default and the burden of proof is on the alternative. It has
been discharged three times, and the three are worth listing together because
the reasoning is not the same each time — "we wrote it ourselves" is not a
policy here, it is three separate findings.

- **An LZ4 *block* decompressor, in `mono.engine/bake/src/RobloxModel.cpp`.**
  The argument is size against surface: an LZ4 block has no framing, no
  checksum and no dictionary, so the decompressor is a hundred lines and the
  submodule declaring it would be larger. **Zstandard is the same job and is
  vendored**, because a Zstandard frame is entropy coding and a real library.
  The two sit five lines apart in one file, which is the best place for a rule
  like this to be visible.
- **XML, in `mono.engine/game/src/Xml.cpp` at v0.7.** A save file is a document
  a player can be sent, and the famous XML attacks — entity expansion, external
  entities, quadratic blowup — are all attacks on features a save file does not
  need. A parser that *has* those features and turns them off is safe by
  configuration; one that never had them is safe by construction. That file
  states the rule it lives under: it is not a general XML library and must not
  grow into one.
- **XML again, in `mono.engine/bake/src/Xml.hpp` at v0.15**, for `.rbxmx` and
  for the SVG rasteriser that had its own scanner already. The finding here is
  the one that is easy to miss: **vendoring would not have removed a
  hand-written parser, it would have added a library beside one.** `bake` could
  not call `game::ParseXml` — `game` is L10 and `bake` is L9 — so the real
  choice was one scanner in that module or a submodule *and* a scanner.

**That third one is now one, and the way it got there is the point.** `D00128`
closed later in v0.15 by moving the scanner **down** rather than by vendoring:
`mono.engine/core/include/engine/core/Xml.hpp` is L1, every caller links `core`
already, and the three copies became one reader plus one save-format document
model plus one writer. It deleted code and added no dependency, which is what
the entry predicted. The tier was the whole obstacle, and the height it landed
at is the height of what the parser needs — nothing but the standard library.

Two things about a consolidation that are worth carrying to the next one:

- **Two callers may need different refusals, and the shared thing states both
  rather than picking.** The SVG rasteriser sweeps a whole document for entity
  references because it never unescapes; the `.rbxmx` reader refuses at each
  point one is read with CDATA exempt, because a real model in the corpus
  carries the Luau pattern `"[&;]"` inside a script. Collapsing those into one
  policy refuses a valid file, and each has a case that goes red if somebody
  does.
- **The writer did not move with the reader.** It writes one format's dialect,
  it has one caller, and no caller below L10 is possible. A shared thing with
  one caller is a second place to keep that format true.

The trade each of these accepts is real: a hand-written parser over hostile
input is a liability. What pays for it is that the grammar is small enough to
read, that every count is checked before it is used, and that the attacks have
tests of their own rather than a note saying they were considered. **If any of
those three stops being true, vendoring is the answer again.**
