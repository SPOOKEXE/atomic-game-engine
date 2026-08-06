# mono.cdn — module invariants

The content origin: a `shared`-tier library and a thin main over it.
`repo_layout.md` §11 is the design; this file is what a reviewer should refuse.

## The short link row is the product

`mono.cdn` links `core`, `assets`, `delivery`, `net` and `parallel`. No `ecs`,
no `scene`, no `script`, no `world`, no `render`.

**Two of those arrived at v0.9 and both are changes to §8 rather than drift
within it.** `net` is the wire — `ROADMAP.md` v0.9 said in advance that building
the origin's HTTP hop would move this row. `delivery` is where `GroupCodec` went,
because a compressed group is a format with two ends and a copy on each side is
how a format acquires a dialect. Anything added from here is a change to §8 too,
and it goes in `expected_graph.json` in the same commit.

That is not tidiness. It is the reason `cmake --preset cdn` configures on a bare
container with no Vulkan SDK, no SDL and no shader compiler — and the day that
stops working is the day somebody added an edge that should not exist.

**The tier does the enforcing, and it is stricter than the server's.**
`MONO_TIER_ALLOWS_shared` is `shared` alone, so a `client`-tier or `server`-tier
target on this link line fails the configure with the edge named. The server
gets its headlessness from a build-option guard plus a staged-directory check;
this program gets it from the tier rule itself, which cannot be switched off by
a preset.

If something here needs `ecs`, the design has drifted. The origin moves bytes it
does not interpret.

## A dependency is declared when it has a caller

Each of the three arrived with its first caller rather than ahead of one:
`assets` when `Gate` began opening grants, `parallel` when `Origin` began fanning
group compression out with `Jobs::For`.

Keep doing that. A dependency declared ahead of its first caller is
indistinguishable from a mistake, and on the one program whose short link row
*is* the point, an unexplained edge is the most expensive kind. `net` and
`delivery` arrived the same way at v0.9: `net` when `Service` began listening,
`delivery` when `Origin` began compressing against a codec both ends share.

**Every entry is still `shared`**, which is the property the row exists for.
`just check-cdn-is-bare` is the standing proof and is the thing to run when this
line is edited again.

## The origin decides nothing about who

`cdn::Gate` opens a grant and answers whether one bundle may be served. That is
the whole of the origin's authorisation surface, and it must stay that size.

**No accounts, no sessions, no player data.** The server is the only thing that
knows the session, the player, what they have loaded and what they are entitled
to. An origin that grows a lookup table for any of it is a second authority, and
two authorities that can disagree eventually do — usually under load, which is
when it is hardest to see.

`Gate::Admits` takes a **content hash**, never a path. There is no overload
taking one and there must not be. `ContentRoot` does traversal checking properly
and is the build side; a request path taking a path would have to repeat that
check, and a repeated check is one that will eventually differ.

**A refusal returns a boolean, not a reason.** The counters tell an operator
which check failed; a reason returned to a client is an oracle.

The MAC and expiry checks are `Engine::assets`' and are not repeated here. A
second implementation of a security check is the one kind of duplication that
reliably gets one copy updated and not the other.

## `CDNSettings` is the whole of an origin's setup

One type, not several. The three deployments `CDN.md` §6 names are field
combinations rather than three programs or three classes:

| Deployment | Settings |
|---|---|
| Local store — a server serving its own disk | `AllowUpstream` off |
| Cache server — local first, forward on a miss | `LocalFirst` + `AllowUpstream` on |
| Pure proxy — always ask upstream | `LocalFirst` off, `AllowUpstream` on |

Do not add a second settings type or a `ProxyOrigin` class. The moment the
deployment is a *type* rather than a field, moving between them is a rebuild
rather than a configuration change — which is the thing §11 says must stay a
deployment decision.

**Local first is the default and it is what makes this a cache.** A hit costs a
lookup and no network at all.

**`AllowUpstream` is off by default.** An origin that will fetch from elsewhere
is an origin that can be pointed at elsewhere, so turning it on is a deliberate
act rather than something a default arranges.

**Forwarding with no upstreams configured is refused at validation.** It reads as
"this will forward" and behaves as "this refuses every miss", and the gap between
those is a deployment that looks healthy and serves nothing.

**Attempts are bounded.** A request that walks ten dead upstreams spends ten
timeouts before it refuses, and the client gave up long before.

## What an upstream returns is checked before it is trusted

`VerifyUpstream` compares what arrived against what the **signed** manifest
records that bundle weighs. A proxy that forwards bytes it cannot check is a
proxy that launders a compromised upstream.

**Be honest about how much that is.** A length check against signed data is real
and it is not chunk-level verification — that needs the chunk layout inside a
group, which is not designed yet. A client verifies end to end regardless, so
this is defence in depth and the trust boundary is still the client's. Say so
rather than implying the proxy is authoritative.

A bundle the local manifest does not describe is refused outright. This origin
cannot say anything about content it was not published, and caching it would let
an upstream decide what this origin serves.

Failed and rejected are counted apart. An upstream that is down and one that is
serving wrong content are not the same incident.

## An upstream returns uncompressed bytes, never a frame

An upstream's dictionary is not necessarily this origin's. Caching a frame
compressed against someone else's dictionary under this origin's key would hand a
client bytes it cannot decode — the same failure keying on the bundle alone
produces.

## A publication is immutable and publishing is an atomic swap

`Origin::Publish` replaces a `shared_ptr<const Publication>`. It does not mutate
one, and there is no accessor that would let a caller.

**A request is served against the publication it was admitted against**, not
whatever is current when it finishes. Mutating a live publication would hand a
client a manifest naming chunks that are not there yet, and the failure arrives
as a hash mismatch a long way from its cause. The suite pins this by swapping
mid-request.

Publishing clears the prepared cache. The previous publication's groups were
compressed against content and a dictionary that are no longer current.

**A null publish is refused.** An origin with nothing to serve refuses requests
rather than serving nothing, and the two are only distinguishable if that is
stated.

## Concurrency is per request; `Jobs::For` appears exactly once

`Jobs::For` blocks until done. That is right for a tick and wrong for a service
whose work is waiting on a filesystem — a construct that occupies a worker while
it waits turns an IO-bound origin into a thread-starved one.

The one place it *is* right is compressing a set of groups: CPU work with a
known end. It is used there and nowhere else in this module. Do not reach for it
to "parallelise the pipeline".

Three details in that fan-out are load-bearing:

- **The work set is gathered first.** A fan-out over a container something else
  may append to is a data race with a plausible-looking body.
- **Resolving a payload happens *outside* the fan-out.** Reading a filesystem or
  talking to an upstream is IO, and a construct that occupies a worker while it
  waits is exactly what the rule above forbids. `Pump` is four stages — cache
  lookup, resolve, compress, publish — and only the third is fanned out.
- **Cache lookups happen before it**, on one thread. A hit costs a lookup and no
  compression, and doing them inside would put a mutex in every worker.
- **Each worker writes only its own index.** That is what `Jobs::For` requires
  and why inline and pooled execution are observationally identical.

A request cancelled while a pump was compressing has its result discarded and
**not cached** — a group nobody asked for evicts one somebody did.

`Pump` prepares at most `PreparePerPump`. A burst must not make one pump run for
an unbounded time and starve whatever else the calling thread does.

## The cache key is the bundle *and* the dictionary

Both halves. A group compressed against one dictionary is a different artefact
from the same group compressed against another, and serving the wrong one hands a
client bytes it cannot decode. Keying on the bundle alone is the bug that only
appears the day a second dictionary exists.

**A prepared frame is shared ownership, never a view.** Eviction happens on
whichever thread inserts, and a reader streaming a frame must not have it freed
underneath. The alternative — a lock held for the length of a transfer — makes
one slow client block every eviction in the origin.

Inserting a key already present keeps the first frame. Two threads that prepared
the same group raced; preparation is deterministic so both results are
byte-identical, and churning the entry would invalidate whatever is streaming it.

A frame larger than the whole capacity is refused rather than stored by emptying
the cache for it.

## `PayloadSource` is a seam, and v0.9 gave it a caller

The on-disk chunk layout was deliberately not decided here — `CDN.md` §7 listed
it as not started, and wiring the pipeline directly to a filesystem would have
baked an undecided layout into the request path.

It is decided now, and **it is decided in `Engine::assets`** rather than here:
`assets::ChunkStore`, because a publisher writes that tree and a client reads it,
so one implementation or the two acquire a dialect. `Service` closes over a
store and hands `Origin` a source; the seam stays, and what it is wired to is a
`shared` module both ends already link.

A source returning nothing is a **refusal**, never an empty group. Serving an
empty group would hand a client bytes that verify against nothing.

## Compression is per group, and at no other level

Not per file — that loses the cross-file redundancy which is most of the ratio
on many small assets. Not per manifest — that defeats range requests, partial
fetch and the whole hash tree.

**Chunks stay uncompressed at rest.** Dedup and patching work on them, and a
compressed chunk store would trade the thing content addressing exists for
against a ratio the group level already gets. Two levels, two jobs: chunks are
storage, groups are delivery.

**The content checksum is on, and Zstd leaves it off.** With it off a frame with
a flipped byte decompresses cleanly to the right length and the wrong content.
The chunk hashes would still catch that — a client verifies everything against
the signed manifest root — but only after a whole group had been transferred and
expanded, and with nothing distinguishing a bad transport from a bad origin. Do
not turn it off to save four bytes a frame.

**A decompression buffer is sized from the manifest, never from the frame.**
`ZSTD_getFrameContentSize` answers what the *frame* claims, and a frame is
attacker-controlled: a few kilobytes on the wire can declare a multi-gigabyte
payload. The manifest records what a bundle weighs and the manifest is signed.
A frame that decompresses to anything other than exactly that is refused rather
than truncated or padded — padding it would hand the hash check bytes the origin
never sent.

**`Dictionary::Load` refuses anything without a trained dictionary's magic.**
Zstd accepts arbitrary bytes as a "raw content" dictionary, which is legal and
nearly useless; a manifest shipped where a dictionary was expected would then
cost ratio on every group for the life of the deployment, silently.

Training never happens on a serving path. It is a pass over the whole sample and
would make an origin's first response wait for it.

## Grouping has three rules and they are ordered

`Grouper` applies self-sufficiency, then the size mix, then priority. **The
order is the design, not an implementation detail.**

**Rule 1 outranks the size bound.** An affinity is never split, even when it is
heavier than `GroupPolicy::MaximumBytes` — splitting it would produce two groups
neither of which makes anything appear, which is the single outcome this class
exists to avoid. The oversized group is *counted* rather than hidden: a bound
quietly broken reads as a bound that held, and the first anyone hears of it is a
client stalling on a group it cannot stream in time.

**Affinity zero binds nothing.** It means "belongs with nothing in particular",
so each such asset is its own cluster. Treating zero as an affinity like any
other would bind every unrelated asset in the game into one lump.

**Largest-first packing is what produces the mix**, and there is no separate rule
for it. Packing smallest-first would fill whole groups with tiny assets and
strand the large ones alone — every group then pathological in one direction or
the other.

**Grouping is deterministic.** Two origins that group the same content
differently prepare and cache different bundles for it, and nothing anywhere
reports that they have stopped sharing. That is why clusters come out of a
`std::map` rather than a hash map, and why the sort's last tiebreak is the asset
root rather than input order.

Group *sizes* are policy and live here. What a bundle **is** — a root over
sorted asset roots — is `Engine::assets`, and this module must not grow a second
opinion about it.

## Resolution refuses by default

`ContentRoot::Resolve` is the only thing that turns a name into a path, and
every name it sees is untrusted — from a manifest, a session descriptor or a
request. §11 says the descriptor is untrusted content; the names inside it are
untrusted for exactly as long.

Two checks, and **both are load-bearing**:

- Component by component, before the filesystem is touched. Catches `..`, `.`
  and absolute names. Cannot see a symlink.
- Containment of the resolved path against the canonical root. Catches the
  symlink. On its own it would accept a name that only lands inside by
  accident, and accept two spellings of one file.

Do not delete one because the other "already covers it". They cover different
things, and the tests name which is which.

`.` is refused along with `..`, and that is deliberate rather than incidental:
`a/./b` and `a/b` naming one file is two manifest keys for one piece of content,
and the manifest will key on the name.

**Do not add a second path that reads a file.** The moment there is an `Open`
that does not go through `Resolve`, the check above is advisory.

## The root is canonicalised once, at mount

`Mount` fails on a root that is missing or is not a directory. It would be
easier to accept it and let each request fail on its own, and that is precisely
the failure mode worth refusing: a misconfigured deployment then looks like a
stream of individually plausible missing files, at request rate, with nothing
saying the root was wrong.

Canonicalising once is also what lets `Resolve` decide containment by comparing
two paths. A root holding a symlink or a `..` would make containment depend on
how the deployment spelled its own configuration.

## Both halves of the format share one implementation

Manifest parsing, content addressing, chunking and hash verification belong in
`mono.engine/assets/` at `shared` tier — used by the running game to fetch and
verify, and by this program to build, sign and serve. §11.

**Do not write a manifest reader here.** Writing the format twice is how a
format acquires a dialect, and the bug it produces is a client rejecting content
the origin considers valid.

A server serving its own assets is not a fourth implementation either. It is
`Engine::assets` over a local store plus `Engine::net`'s HTTP layer, linked into
`mono.server`. One store, one manifest format, three deployments.

## The origin decides admission; publishing is a different mode for a reason

`cdn --publish` builds a store and `cdn --store` serves one, and they are
separate invocations because **the signing key is separate**. A key belongs to
whoever publishes the game and the origin holds none — that is what makes it
deployable on hardware nobody here owns. A single mode that published on start-up
would put a signing key on every serving box, permanently.

`--grant-key` is required to serve, and it is not a convenience to remove. An
origin that admitted everyone would be deciding who may have what, which is the
server's job.

## The dashboard is a model plus a device, and the model owns no terminal

`--gui` is two headers and the split between them is the design, not tidiness.
`Dashboard.hpp` is arithmetic over a publication and a run of counter samples
producing lines of text; `Terminal.hpp` owns raw mode, the alternate screen and
an escape sequence. **No escape sequence is emitted by the model** — a colour
belongs to a `LineStyle` the terminal reads, because a model that emitted colour
could not be diffed by a test.

That is the same split `net`'s `Message.hpp` has against its `Server`, and it
buys the same thing: key decoding, scroll arithmetic and frame composition are
free functions over values, so everything an operator sees and presses is
covered by a suite that opens no tty. `Terminal::Open` is the one piece with no
unit suite, for `Renderer.hpp`'s reason — it needs the device, and a mock would
close the gap on paper only. Do not add one.

**The live section has a fixed line count.** It is rewritten from every sample
and the content section is written once, so a section that grew a row when a
counter became interesting would slide the asset list under whoever was reading
it, for a reason nothing on screen explains. The suite pins the count.

**Sampling is every pump; composing and drawing are not.** The history is built
from differences between readings, so a reading skipped puts traffic in the
wrong minute — but formatting sixteen lines and two sparklines a hundred times a
second to draw four of them is work nobody asked for. `Sample` records and marks
the section stale; `ComposeLive` runs on the next `LineAt`.

**The history is sixty one-minute buckets and the newest is partial.** An hour
at sample rate is a ring nobody reads at that resolution. The partial minute is
said in the row rather than hidden, because a total that silently includes a
third of a minute reads low for no visible reason. A rate is measured over a
second rather than between two pumps, where one 16 KB read swings the number by
a factor of a hundred between redraws.

**`ServedBytes` and `SentBytes` are two measurements and stay two.** One is the
compressed group payload a client asked for; the other is what the interface
moved, which also carries manifests, dictionaries, health checks, refusals and
every response's headers. An operator watching bandwidth wants the second and an
operator asking what delivery cost wants the first, and a single number cannot
answer both. `Engine::net` counts the second at the socket — see `net/AGENTS.md`.

**The dashboard holds no clock**, the same rule the rest of this module has.
`app/main.cpp` reads two: wall time for a grant, because the server that issued
it used wall time, and `steady_clock` for the dashboard, because a wall clock
that steps back an hour would put an hour of traffic in one bucket and draw a
spike nothing caused.

**Raw mode swallows Ctrl-C on purpose.** Leaving `ISIG` on lets the signal end
the process with the terminal in raw mode, no cursor and the alternate screen
up — a shell nobody can type into, from a program with a perfectly good
destructor. Ctrl-C decodes as `Key::Quit`. Do not "fix" this with a signal
handler.

**`--gui` on something that is not a terminal warns and serves without one.**
Escape sequences in a log file are a log nobody can read, and serving is the
job.

## Not here yet

Named so nobody adds half of one:

- `control/` — the upload API, auth and the web dashboard, in TypeScript,
  talking to this program's HTTP API rather than reimplementing any of it. It is
  not what `--gui` is: that reads this process's own counters over a terminal it
  already has, and a dashboard for a fleet is a different trust boundary.
- Invalidation, and an edge cache to invalidate.
- Chunk-level verification of what an upstream returned. Today it is a length
  check against the signed manifest, which is real and is not the whole of one —
  say so rather than implying the proxy is authoritative.
- Range serving is present but nothing *resumes* with it yet: the delivery
  client fetches whole groups. The route answers `206` correctly, and a client
  that dropped mid-group would want it.

`app/main.cpp` used to mount a root, report it and warn that it served nothing.
That warning is gone with the thing it was warning about.

## Not here at all

Deployment infrastructure — Terraform, Kubernetes, edge configuration — is a
separate private repository, because it holds account identifiers and rotates on
a different schedule. Large sample content is a separate content repository; a
monorepo carrying a gigabyte of test assets makes every clone slow forever.

Keeping this member's contract narrow is what makes it the only one that could
leave the repository without pain. §16 decision 1.

## The directory name

`repo_layout.md` §11 writes this member as `mono.contentdelivery/` while naming
its program, its preset and its staged directory `cdn`. The folder is spelled
`mono.cdn/` to match the three that something checks, so the target, the preset,
the staged tree and `just cdn` are one word.

If §11 is re-spelled to match, this note goes with it. Do not leave two names
for one thing in the tree.
