# mono.vendor — module invariants

Third-party dependencies, shared by every `mono.X` folder that needs one.
There is no per-folder vendor directory and there should never be.

## Submodules only. No first-party code

Not one file. Vendoring a third-party source as a copy works once and then
drifts invisibly — the copy gets a local fix, upstream gets a different one,
and nobody finds out until a version bump three years later.

If a dependency needs a patch, the patch goes upstream or into a fork whose
remote is recorded in `.gitmodules`. It does not go into a file in this tree.

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
