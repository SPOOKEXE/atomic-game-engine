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
just serve --root ./content       # a directory of your own
./.cache/build/dev/cdn/cdn --help
```

```
--root DIR   Directory to serve content from (default: beside the binary)
--verbose    Log at trace level
--help       Show this text
```

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

HTTP range serving is `Engine::net`. The upload API, auth and dashboard are
`control/`, in TypeScript, talking to this program's HTTP API rather than
reimplementing any of it.

Until those land the program mounts a root, reports it and warns that it serves
nothing. ROADMAP.md v0.8.
