# The C++ linker, as this repository experiences it

**Why this file exists.** Two places in the tree make a claim about the linker —
`mono.vendor/AGENTS.md` says a vendor costs nothing until something links it, and
`THIRD_PARTY_NOTICES.md` says a shipped client contains none of shaderc because
"the linker drops every object". Both claims are load-bearing: the second one
decides which licence notices a distribution owes. Neither was written down with
a number next to it.

This is the measured version. Every figure below was produced on this tree, and
the command that produced it is next to it so it can be re-run rather than
believed. Figures are from the `ci` preset (`-g -O0`) unless a line says
otherwise, so treat them as ratios rather than as shipping sizes.

---

## 1 · A static library is an archive of objects, not a library

`libcryptopp.a` is 173 `.o` files in a `tar`-like container. The linker does not
"link the library". It walks its inputs left to right holding a set of undefined
symbols, and for each archive it pulls in **only those members that define a
symbol currently undefined** — then repeats, because a pulled member brings its
own undefined symbols with it.

The unit of that decision is **the object file**, not the function. One used
function drags in every other function in the same `.cpp`.

```sh
# What is in the archive, versus what a SHA-256-only program actually took.
ar t .cache/build/ci/lib/libcryptopp.a | wc -l
g++ -O2 -isystem mono.vendor prog.cpp .cache/build/ci/lib/libcryptopp.a \
    -o prog -Wl,-Map=map.txt
grep -oE 'libcryptopp\.a\(([a-z0-9_]+)\.cpp\.o\)' map.txt | sort -u | wc -l
```

| | |
|---|---|
| Members in `libcryptopp.a` | **173** |
| Members linked into a program that calls only SHA-256 | **36** |

So the claim holds: 137 members — ARIA, Camellia, Blowfish, Twofish, most of the
cipher catalogue — never enter the binary. **This is the mechanism behind the
tier and notice reasoning in `THIRD_PARTY_NOTICES.md`, and it is real.**

## 2 · But dead-stripping has a floor, and it is higher than you expect

Those 36 members are not the three you would predict. The full list:

```
algparam allocate asn basecode cpu cryptlib dll ec2n ecp filters fips140
gf2n gf2n_simd gfpcrypt hex hmac hrtimer integer iterhash misc modes mqueue
nbtheory oaep osrng primetab pubkey queue randpool rdtables rijndael
rijndael_simd rng sha sha_simd sse_simd
```

A program that computes one SHA-256 digest links **elliptic curves** (`ecp`,
`ec2n`), **arbitrary-precision integers** (`integer`, `nbtheory`, `primetab`)
and **AES** (`rijndael`). Not because SHA-256 calls them, but because
`cryptlib.o` is unavoidable, and `dll.o` and `fips140.o` carry static
initialisers that reference Crypto++'s algorithm registry. A static initialiser
is an undefined-symbol source the linker cannot reason away — the object is
either in or out, and the registry keeps it in.

**The practical rule: reason per archive-member, and expect a per-library floor
you do not control.** "We only call one function from it" is not a size
argument.

## 3 · What actually reduces it

Measured on the same program, built `-O2` rather than with the `ci` preset:

| Build | Size | `CryptoPP::` symbols |
|---|---|---|
| Plain | 8.3 MB | 9,475 |
| `-ffunction-sections -fdata-sections -Wl,--gc-sections` | 6.8 MB | 4,432 |
| …and then `strip` | **1.1 MB** | — |

Two things worth taking from that table:

- **`--gc-sections` moves the granularity from object to function**, and halves
  the symbol count. It is the tool that actually addresses §2's floor.
- **Most of the 8.3 MB was never code.** Symbols and debug information dominate;
  the executable content is about 1.1 MB. Quoting an unstripped size as the cost
  of a dependency overstates it by roughly seven times, which is worth
  remembering before anyone argues from a `du` output.

Neither flag is on in this repo today. That is a deliberate non-decision:
nothing here is size-constrained yet, and `--gc-sections` interacts badly with
patterns worth staying awake for (§6).

## 4 · `VENDOR` and `VENDOR_PUBLIC` decide compilation, not linkage

This is the one that surprises people, and it is worth being exact about because
the names suggest otherwise.

`mono_add_library(... VENDOR Vendor::cryptopp)` links the vendor `PRIVATE`. For
a **static** library CMake still records that in the target's interface as
`$<LINK_ONLY:...>`, because a static archive does not absorb its dependencies —
anything linking `engine_core` must also link what `engine_core` needs.
Measured on `Engine::core`, which owns Crypto++ privately:

```sh
ninja -C .cache/build/ci -t commands client | grep -oE "lib/libcryptopp\.a"
ninja -C .cache/build/ci -t commands client | grep Demo.cpp | grep isystem
```

| | `client` |
|---|---|
| `libcryptopp.a` on the **link** line | **yes** |
| cryptopp on the **compile** line | **no** |

So `VENDOR` versus `VENDOR_PUBLIC` controls **who compiles against the vendor's
headers**, and nothing else. It does not keep the code out of the binary — §1
does that, by not referencing it.

The reasons to prefer `VENDOR` are therefore unchanged and still good: it keeps
a vendor's types out of our public headers, keeps compile times down, and keeps
the module replaceable. It is simply not a size argument or a distribution
argument, and should not be written up as either.

## 5 · Link order is not always cosmetic

Because the linker walks its inputs left to right (§1), an archive placed before
the object that needs it contributes nothing — the symbol is not undefined yet.
CMake normally gets this right; hand-written link lines do not always.

Crypto++ adds a second, sharper case. Its `GNUmakefile` hard-orders the first
three objects:

```make
SRCS := cryptlib.cpp cpu.cpp integer.cpp $(filter-out ...,$(sort $(wildcard *.cpp)))
```

That is a **static initialisation order** dependency, not a symbol-resolution
one: file-scope objects in `integer.cpp` need `cpu.cpp`'s CPU-feature detection
to have run first. Get it wrong and there is no link error — there is a binary
that works until it runs on a machine with a different instruction set.

This is the strongest single reason `mono.vendor/cryptopp-cmake` is vendored
rather than the target being declared by hand in `MonoVendor.cmake` the way asio
and imgui are. `.gitmodules` carries the rest of that argument.

## 6 · Things that quietly defeat the above

- **Static initialisers**, as in §2. An object carrying one is pulled in
  whenever its archive is consulted, and `--gc-sections` will not drop it: the
  initialiser is reachable from `.init_array` by construction.
- **Virtual functions.** A class with a vtable keeps every virtual it declares,
  because the vtable references them all. That is much of why §2's floor sits
  where it does — Crypto++ is a deeply polymorphic hierarchy.
- **`--gc-sections` needs `KEEP`.** Anything reached only from a linker script,
  an `__attribute__((used))`, or a register-at-static-init pattern has to be
  kept explicitly. Turning the flag on repo-wide is a real change, not a free
  win, which is why §3 stops at measuring it.
- **`EXCLUDE_FROM_ALL` is a build-graph property, not a link one.** It stops a
  target being built by a bare `ninja`. The moment something links it, it is
  built, and §1 applies as normal. `Vendor::cryptopp` sat behind it doing
  nothing until `Tool::testrunner` named it.

## 7 · The `mono.vendor` include-path hazard

Not linkage, but it is found the same way and it appeared during the Crypto++
work, so it belongs beside this.

`cryptopp-cmake` puts the **parent** of the Crypto++ source directory on its
`PUBLIC` include path so that `<cryptopp/sha.h>` resolves. In this tree that
parent is `mono.vendor/` itself. Any target linking `Vendor::cryptopp` therefore
has `mono.vendor/` on its include path and can reach `<glm/...>`, `<sdl/...>`
and `<flecs/...>` whether or not it links them:

```
-isystem /…/mono.vendor/cryptopp
-isystem /…/mono.vendor          ← this one
```

**Write `<cryptopp/sha.h>`, and treat the bare `<sha.h>` spelling as
unavailable.** A file that compiles only because of this path stops compiling
when Crypto++ moves, and the failure will look unrelated to the cause.

`MonoVendor.cmake` marks all of it `SYSTEM`, so a warning inside a vendored
header can at least never fail the `ci` preset's `-Werror`.

## 8 · How to check any of this yourself

```sh
# The link line for a target, and which archives are on it.
ninja -C .cache/build/ci -t commands client | tail -1

# Symbols from a given vendor that survived into a binary.
nm -C .cache/build/ci/client/client | grep -c 'CryptoPP::'

# What a target compiles against, as opposed to what it links.
ninja -C .cache/build/ci -t commands engine_core | grep isystem

# Which archive members a link actually pulled in.
# (add -Wl,-Map=map.txt to the link, then:)
grep -oE 'lib[a-z0-9]+\.a\([a-z0-9_]+\.cpp\.o\)' map.txt | sort -u
```

The `nm | grep -c` form is the one that settles distribution questions. It is
how `THIRD_PARTY_NOTICES.md` arrived at "687 shaderc symbols in `test_render`,
zero in `client`", and re-running it is the correct way to check that claim is
still true after a version bump, rather than assuming it.
