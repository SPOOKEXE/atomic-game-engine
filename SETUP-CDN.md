# Setting up content delivery

How to get your own art into a game, three ways, from the one that needs no
server to the one that serves other people's machines. Every command here was
run against this build; the output shown is what it printed.

[`RUNNING.md`](RUNNING.md#the-content-origin) is the reference for every flag.
This is the walkthrough.

---

## The one thing to understand first

**A client trusts the manifest, not the origin.** Content is fetched by name,
every name comes from a manifest, and a manifest is signed. That is why three
different keys turn up below and why none of them is optional:

| Key | Held by | What it decides |
|---|---|---|
| **signing key** | whoever publishes | who signed this content |
| **publisher key** | every client | whose signatures it will accept |
| **grant key** | the origin and the server | who may spend the origin's bandwidth |

The publisher key is the public half of the signing key, and `cdn --publish`
prints it. A client with no publisher key fetches **nothing** - not "everything
unverified", nothing at all.

**A runtime does not decode.** It reads `.atex`, `.amesh` and `.amat`, never
`.png` or `.gltf`. Something has to bake first, and that something is `assetc`
or the editor. Publishing a folder of PNGs succeeds, serves, verifies, and
delivers files nothing can read - which looks exactly like a broken renderer.

---

## 1. A folder on your own machine

The smallest thing that works. No server, no ports, no keys to type.

### Through the editor

1. Open the editor: `just build studio && ./.cache/build/dev/studio/studio`
2. Drop files anywhere on the window, or **Assets → Add files…**. They land in
   `~/Documents/atomic-game-engine/cdn/raw/`.
3. Press **Publish**. That bakes `raw/` into `baked/` and signs a manifest into
   `processed/`.
4. Everything already points at it: the editor's first content source is that
   folder, and the client defaults to the same one.

The signing seed the panel asks for is the only awkward part, and only because a
publish signs. For the store on your own disk there is a development identity
built in - `cdn::DevelopmentSigningKey` - which is what `contentimport` uses when
you give it no key:

```sh
./.cache/build/dev/tools/contentimport ~/art/props --publish
```

That identity is **not a secret and not for a deployment**. It is a well-known
key for a well-known folder, so that a local store is not sixty-four characters
of friction. An origin serving other machines supplies its own.

### Raw folders, with no publish at all *(v0.14)*

For "I am still moving the wall around", there is one step less. In
**Preferences → Content → Raw folders**, add the directory your art is in. The
assets panel grows a tab for it, listing every file under the name it *would*
have once baked - `props/crate.png` shows as `props/crate.atex` - and a **Load**
button bakes that one file and hands it to the viewport.

**Memory-only is on by default**, which means nothing is written anywhere: not
into your store, not beside your art. Turn it off and each load also writes into
the store's `baked/`, ready to publish.

The names do not change when you graduate. A scene that names `props/crate.atex`
while the folder is memory-only names exactly the same thing once the file has
been imported and published, which is what makes this an authoring shortcut
rather than a different content system.

---

## 2. A store, served from a folder, still with no server

A published store is a directory. Anything that can read the directory can be a
source, so a store on a shared drive needs no origin process at all:

```sh
# Bake source art into a tree a publisher can read
./.cache/build/dev/tools/assetc --input ./content --output ./baked
# assetc: logo.png -> logo.atex [texture] 6290085 bytes
# assetc: 2 assets, 0 failed

# Publish it, signing with a key you chose
./.cache/build/dev/cdn/cdn \
    --publish ./baked \
    --store   ./store \
    --signing-key $(printf '7a%.0s' {1..32})
# cdn: published 2 assets in 1 bundles
# cdn: manifest root a542fa81143e...
# cdn: publisher key ba42458e83ba...
```

Point anything at the folder with `dir:`:

```sh
just run --cdn dir:./store --publisher-key ba42458e83ba...
# content: 1 source(s), first is 'dir:./store'
# delivery: catalogue from 'dir:./store' - 2 assets, 1 bundles
```

Republishing is cheap - content already in the store is a no-op - so the loop of
"bake, publish, look" costs what changed.

---

## 3. An origin on localhost

Same store, now over HTTP. This is the deployment shape, run on the machine you
are sitting at.

```sh
./.cache/build/dev/cdn/cdn \
    --store      ./store \
    --grant-key  $(printf 'ab%.0s' {1..32}) \
    --port       9080
# cdn: serving on 0.0.0.0:9080
```

`--grant-key` is **required**, and it is not a formality to remove. Delivery
costs bandwidth, and who is allowed to spend it is a decision the *server* makes
by issuing grants - an origin that admitted everybody would be making that
decision itself, which is the one thing it must not do.

Check it with anything:

```sh
curl http://127.0.0.1:9080/health          # ok 2 assets 1 bundles
curl -o manifest.acm http://127.0.0.1:9080/manifest
```

And fetch:

```sh
just run --cdn 127.0.0.1:9080 --publisher-key ba42458e83ba... --content-cache ./cache
# content: 1 source(s), first is '127.0.0.1:9080'
# delivery: catalogue from '127.0.0.1:9080' - 2 assets, 1 bundles
```

In the editor the same list is **Preferences → Content**. Add a row, kind
`http`, location `127.0.0.1:9080`. The order of the rows is the priority - the
first source that answers wins, and one that fails is passed over - so a local
folder above a remote origin *is* "cache locally, otherwise ask".

### Watching it

```sh
./.cache/build/dev/cdn/cdn --store ./store --grant-key HEX --port 9080 --gui
```

A terminal dashboard: bytes in and out at the socket, requests by kind, what is
in the store, and the largest assets. `q` leaves it and restores the terminal.

### Taking uploads *(v0.14)*

An origin can accept content as well as serve it, which is what the editor's
**Upload** button targets. It is off until a secret is given:

```sh
./.cache/build/dev/cdn/cdn --store ./store --grant-key HEX \
    --ingest-key letmein --inbox ./store/inbox
# cdn: accepting uploads at /ingest into ./store/inbox
```

Then in **Preferences → Content**, give the row role `write` and paste `letmein`
into its ingest key.

**Uploading and publishing stay two acts.** What lands in the inbox is unsigned,
and no client will look at it until a publisher has signed a manifest naming it.
That is why the ingest secret is only an admission check: whoever holds it can
spend this origin's disk, and cannot store anything under a name that is not its
own true content hash - the body is hashed and compared against the address it
was uploaded to. A wrong key is `403`; on an origin with no `--ingest-key` the
route answers `404`, as though it were not there.

### Letting it be listed *(v0.15)*

An origin serves by name, so nothing about fetching needs it to say what it
holds. The editor's assets panel wants exactly that, though - a tab per origin
is not much of a tab if it cannot show the origin's contents - so there is one
route that enumerates, and it is off until asked for:

```sh
./.cache/build/dev/cdn/cdn --store ./store --grant-key HEX \
    --ingest-key letmein --list-contents
# cdn: enumerating contents at /catalogue, 256 a page, against the ingest key
```

Then paste the same `letmein` into that row's ingest key in **Preferences →
Content**, and the row's tab in the assets panel lists what that origin holds.

**Off by default, and it is the default that matters.** An open write route
costs an operator disk; an open listing hands whoever asks the name of
everything here, and names that have been scraped cannot be un-scraped. So it is
one flag, admitted by the key that already exists - `--list-contents` without
`--ingest-key` refuses to start rather than enumerating for anybody.

`GET /catalogue` answers a page and a `next` cursor to follow;
`GET /catalogue/<cursor>` is the next one. Without the key it is `403`, and on
an origin started without the flag it is `404`, as though the route were not
there - which is why the editor's tab says "switched off there, or an older
build" rather than claiming to know which.

---

## 4. Reaching it from another machine

The origin already binds every interface - the log line says `0.0.0.0:9080` -
so on a LAN there is nothing more to do than use the machine's address:

```sh
just run --cdn 192.168.1.20:9080 --publisher-key ba42458e83ba...
```

For anything past that, in rough order of how much you have to trust:

**A tunnel or an overlay network** - Tailscale, WireGuard, `ssh -R` - is the
honest first answer for "my friend needs my content". The origin stays bound to
a private address and the network decides who is on it.

**A reverse proxy** in front, if it should be public. Terminate TLS there, and
keep these in mind:

- The origin speaks **plain HTTP**. It has no certificate handling, so anything
  reachable from the internet wants nginx, Caddy or a CDN in front of it.
- Content is **content-addressed and signed**, so a proxy cannot alter it
  undetected - a modified chunk fails the hash and a modified manifest fails the
  signature. What a proxy *can* see is which assets somebody fetched.
- The routes are `GET /health`, `GET /manifest`, `GET /bundle/<root>`,
  `GET /catalogue[/<cursor>]` and `PUT|HEAD /ingest/<hash>`. If uploads are not
  wanted from outside, do not proxy `/ingest`; if the contents should not be
  enumerable from outside, do not proxy `/catalogue` - though an origin started
  without `--list-contents` answers it `404` anyway.

**A cache in front of a cache.** Two flags make a second origin a mirror that
fills itself on demand:

```sh
./.cache/build/dev/cdn/cdn --store ./edge --grant-key HEX --port 9080 \
    --allow-upstream --upstream home=203.0.113.10:9080
```

Local first, forward a miss, keep what came back. Dropping `--no-local-first`
and `--no-cache-upstream` in is how you get a pure proxy that keeps nothing.
Same program, three deployments, no separate build.

**Being found without an address.** `--advertise` announces the origin on the
local subnet; `--rendezvous HOST:PORT` registers it with a rendezvous point so
peers elsewhere can find it. Both gate *discovery* and neither replaces the
grant key, which still gates delivery.

---

## When it does not work

| What you see | What it usually is |
|---|---|
| client fetches nothing, says nothing | no `--publisher-key` - nothing can be verified, so nothing is requested |
| `catalogue from …` then no assets registered | the world names no published asset; assets are fetched as they are named |
| assets arrive and draw as the fallback cube or a magenta checkerboard | the store holds source art, not baked art - run `assetc` first |
| `no content store at …` | `--store` points somewhere nothing was published into |
| `403` on upload | wrong ingest key |
| `404` on upload | the origin was started without `--ingest-key` |
| an origin's tab in the assets panel says it does not list | the origin was started without `--list-contents` |
| that tab says the key was refused | the row's ingest key is not the origin's `--ingest-key` |
| editor lists a store as empty | it has never been published - `raw/` is not the manifest |

The editor's **Network** panel is the readout for everything above: sources
tried, what verified, what was refused, bytes moved.
