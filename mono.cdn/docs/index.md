# mono.cdn — the content origin

A `shared`-tier library and a thin main over it, serving a game's content out of
a directory. `repo_layout.md` §11 is the design;
[`AGENTS.md`](../AGENTS.md) is what a reviewer refuses.

Two deployments, one program:

| Runs | For |
|---|---|
| Locally, beside the game | single-player, LAN, split-screen, development |
| Remotely, on its own | a large content collection, at scale |

Nothing in the program distinguishes them. The difference is which directory it
mounts and who reaches it — which is what keeps the choice a deployment decision
rather than a build-time one.

## Three sources, one client path

A running game must not know which origin it is talking to. `Engine::assets` has
one fetch path and a source descriptor behind it, and the server chooses per
session:

| Source | Who serves it | When |
|---|---|---|
| Local store | the server, from its own disk | self-hosted, small games, development |
| Server-proxied | the server, forwarding from a delivery service | the origin is hidden or controlled |
| Direct | a delivery service the client connects to | the common case at scale |

A server serving its own assets is **not** a fourth implementation. It is
`Engine::assets` over a local store plus `Engine::net`'s HTTP layer, linked into
`mono.server`. `mono.cdn` is the same content store deployed on its own. One
store, one manifest format, three deployments.

## The link row

`core` today; `core`, `parallel` and `assets` at the destination §8 records.
No `ecs`, no `scene`, no `script`, no `render`.

That row is the product, not a side effect of it: it is why the `cdn` preset
configures on a container with no Vulkan SDK, no SDL and no shader compiler. The
`shared` tier enforces it — a `shared` target may link only `shared`, so a
presentation module on this link line fails the configure with the edge named.

## Running it

```sh
just cdn                          # build the program and only its dependencies
just serve                        # build and run it against the staged directory
just serve --store ./store --grant-key HEX --gui
./.cache/build/dev/cdn/cdn --help
```

`RUNNING.md` carries the whole flag list. The two modes are one program because
the signing key is separate: `--publish` builds a store and needs a signing key,
serving needs a grant key and holds no signing key at all.

## Watching it serve

`--gui` draws a scrolling terminal view of the origin — what is published, what
it weighs per kind and per asset, the five largest, and traffic in and out as a
live rate, an hour of one-minute buckets and a running total.

Two headers and the split between them is the point:

| Header | Owns |
|---|---|
| `cdn/Dashboard.hpp` | the arithmetic and the text. No terminal, no escape sequence |
| `cdn/Terminal.hpp` | raw mode, the alternate screen, a key, a frame |

Key decoding, scroll arithmetic and frame composition are free functions over
values, so everything an operator sees and presses is covered by a suite that
opens no tty. `Terminal::Open` is the one piece with no unit suite, for the same
reason `Renderer.hpp` has none: it needs the device, and a mock would close the
gap on paper only.

Bytes in and out are counted by `Engine::net` **at the socket**, which is why
`http::ServeReport` carries them: a count taken from parsed requests reports
zero for a peer loading a connection buffer and never finishing a message, which
is the traffic worth seeing.

`cmake --preset cdn` is the bare configure: no client, no server, no graphics
stack. It is the one worth running in CI on a container with nothing installed.

## What is here

| Header | Is |
|---|---|
| `cdn/ContentRoot.hpp` | the boundary between a content name and the filesystem |

`ContentRoot::Resolve` is the only thing that turns a name into a path, and it
refuses by default — every name it sees arrives from outside the process. Two
checks, both load-bearing: components before touching the disk, and containment
of the resolved path against the canonical root. The first cannot see a symlink;
the second alone would accept a name that lands inside by accident.

## What is not here

The manifest, content addressing, chunking and hash verification belong in
`mono.engine/assets/` at `shared` tier, so the running game and the origin share
one implementation. Writing the format twice is how a format acquires a dialect.

HTTP range serving is `Engine::net`. The upload API, auth and the web dashboard
are `control/`, in TypeScript, talking to this program's HTTP API rather than
reimplementing any of it — **`--gui` is not that and does not grow into it.** It
reads this process's own counters over a terminal it already has; a dashboard
for a fleet is a different program with a different trust boundary.
