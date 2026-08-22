# delivery - module invariants

L11 `shared`. Getting content from wherever it is to whoever asked for it: the
source list, the local cache, the group codec and the fetch path. `CDN.md` is
the design; this file is what a reviewer should refuse.

## Why this module exists at all

`repo_layout.md` §5.2 files "remote fetch" under `assets` at L8. **That cannot
be built.** Fetching needs a transport, the transport is `net` at L11, and rule
1 says a layer may see every layer below it and none above it. `assets/AGENTS.md`
reaches the same conclusion from the other side - *there is no network in this
module and there will not be*.

So the fetch path sits above both, and `assets` stays the format. If §5.2 is
re-spelled to match, this note goes with it; do not leave two accounts of where
fetching lives.

**It is not part of `mono.cdn` either.** The origin is a product and this is the
half a game links, so an engine module depending on a program's library would be
the wrong direction. The split is: `assets` is what content *is*, this is what a
group is *in flight* and how one is fetched, and `mono.cdn` is the origin's
policy, its admission gate and its serving. **`mono.cdn` links this, never the
reverse.**

## The completion becomes visible when the caller pumps, and at no other moment

This is the invariant the whole shape of `AssetClient` exists for. A fetch
issued from a world lives inside a tick, and rule 5 has no exception there: a
chunk that becomes visible to a system mid-tick is a desync, because two
machines whose networks happened to differ would then simulate different things.

So there is no callback, no future, no completion handler and no background
thread. There is `Pump`, and a world calls it at the barrier.

**Do not add a convenience that delivers a result outside `Pump`.** Not a
`WaitFor`, not an `OnReady`, not a "just this once" synchronous fetch. Each of
them is a way for content to arrive at a moment scheduling decided, and the
failure is a desync that reproduces on one machine in ten.

The origin side is the opposite and is allowed to be - CDN.md §3, and the reason
is that an origin has no tick.

## The client trusts the manifest, not the origin

Three checks, in this order, and the order is not negotiable:

1. **The manifest's signature**, against `DeliverySettings::Publisher`. Nothing
   else happens until this passes, because everything below is checked *against*
   the manifest and an unverified manifest checks nothing.
2. **A group's decompressed length**, against what the signed manifest records
   that bundle weighs. Sized from the manifest and **never** from the frame - a
   frame header can claim any size, so believing it is a decompression bomb.
3. **Every asset cut out of that group**, against its root.

A source failing any of them is passed over and the next is tried. That is what
makes a compromised, misconfigured or stale origin able to *withhold* content
and unable to *substitute* it, which is the property that makes third-party
delivery safe at all.

**An unset publisher key is refused rather than read as "trust anything".** A
client that accepts an unsigned manifest has no trust boundary, and that failure
is invisible until somebody is serving content the publisher did not write.

## Verification goes through `assets::VerifyAsset` and nowhere else

An asset root is a **tree over chunk hashes**, not the digest of its content. So
`Hasher::Of(bytes) == asset.Root` is wrong for every asset cut into more than one
chunk and *right by coincidence* for some that were not - the worst shape a check
can have, because it passes in the small case somebody tests with.

That mistake was made here once, in `ContentCache`, and it did not fail loudly:
it refused every multi-chunk asset as a cache miss, so every fetch went to the
network for ever and nothing said why. The end-to-end run caught it; no unit test
on either side would have.

**Every call takes the manifest's entry rather than a bare hash**, which is what
makes the correct check the only one that can be written.

## The source list is ordered and the order is the policy

"Local cache first, then the origin next door, then the one across the internet"
is not implemented anywhere. It is what a list in that order *means*: the client
walks it and stops at the first source that answers.

**Do not add a strategy, a policy enum or a mode.** The moment there is a
`FetchPolicy::PreferLocal` beside the list, there are two places that decide the
order and they will disagree.

The cache is consulted before any source and is **not** itself a source. It holds
content this client has already verified, so reading it is not a fetch and cannot
be pointed at somebody else's bytes.

### The cache's running total is a trigger, and must never become a listing

`ContentCache` keeps how many bytes it believes its directory holds. **It decides
whether `MakeRoom` walks the directory at all; it never decides what to evict.**
That distinction is the whole design and a reviewer should refuse a change that
blurs it.

Storing used to walk the whole cache tree every time - three syscalls per file
already there, so caching N assets cost N squared stats, inside `Pump`, at the
tick barrier. Measured over 1024 four-kilobyte assets: 1.17 ms per store, against
16.3 us for the same stores into an uncapped cache, which does no eviction work
at all. It is 16.4 us now.

A total can drift and an index of *what is here* must not. Another process
writing into the directory makes the total too small, so the ceiling is crossed
late by whatever that process wrote; one deleting files makes it too large, so a
walk happens early and finds nothing to do. **Neither can hide a file from
eviction**, because eviction chooses from the walk, and both correct themselves
at the walk, which replaces the total with what it found. A remembered listing
could not make that trade: a stale entry would be a file nothing ever evicts,
which is the directory growing past its ceiling for ever with no symptom.

Eviction goes an eighth of the ceiling below it rather than exactly to it. A
cache smaller than its game's working set sits at its ceiling for ever, and
evicting the minimum puts the walk back on every store - 647 us per store against
36 us with the headroom. What that costs is an eighth of the cache dropped
earlier than it had to be, which is content refetched rather than content lost.

## A descriptor is untrusted

`repo_layout.md` §11: *a client that is told to fetch from an arbitrary host is a
request-forgery primitive.* A source list assembled from anything a server sent
must fill in `AllowedHosts`, and the check lives with the list rather than at the
call sites - a call site added later is a call site that forgot.

Empty means no restriction, which is right for a list a person typed into their
own preferences and wrong for one that arrived over a wire. `HostPermitted` is
exposed so the studio can say *why* a row is refused while somebody is typing it,
rather than re-implementing the rule and eventually differing.

**`Endpoint::Parse` refuses a host name on purpose** and this module does not
work around it. Resolving one is a blocking call to a network service, and
nothing on this path may block; a name is resolved by whoever wrote the
configuration.

## The group codec is here because a group has two ends

`GroupCodec` and `Dictionary` moved out of `mono.cdn` at v0.9. An origin
compresses a group and a game expands one, so a copy on each side is how a format
acquires a dialect - which `CDN.md` §6 refuses twice for the manifest and which
applies identically here.

**Chunks stay uncompressed at rest and compression is per group.** Not per file,
which loses the cross-file redundancy that is most of the ratio on many small
assets; not per manifest, which defeats range requests and partial fetch. Two
levels, two jobs: chunks are storage, groups are delivery.

**The content checksum is on and Zstd leaves it off.** With it off a frame with a
flipped byte decompresses cleanly to the right length and the wrong content. Do
not turn it off to save four bytes a frame.

## What travels, and what a request asks for

The unit that travels is a **group**; the unit asked for is an **asset**. Asking
for one asset fetches the bundle carrying it, and the other members land in the
cache as a consequence - which is the whole of "the game progressively builds"
seen from this end.

A bundle's payload is its member assets concatenated in `BundleEntry::Assets`
order, with no framing and no index. That layout is **derived from the signed
manifest** rather than transmitted, so an origin cannot move an asset's boundary
without breaking the signature. `Manifest::SliceOf` is the one definition of it
and `ChunkStore::ReadBundle` is the one producer.

Cutting a group up asks the manifest per member, twice - once to resolve the
member and once inside `SliceOf` for every member it walks past - so it stays
quadratic in a bundle's size however fast the lookup is. That is affordable
because the lookup is a binary search now: 11.96 us to locate one member of a
32-member group in a 4096-asset manifest before `Manifest::RootOrder` existed,
319 ns after. **Do not answer the remaining quadratic by computing the offsets in
the client.** That would be a second definition of where an asset sits inside a
group, which is the one thing this section refuses.

## How much a frame absorbs is a decision, and it is one decision

`Pump` answers in bursts - a scene naming forty meshes gets forty completions in
the frame the origin catches up - and the expensive part is not taking them. It
is what a caller does next: decoding an `.amesh` and uploading it to the GPU.
A loop that drains every completed request the moment it notices them spends a
third of a second in one frame, and an editor stops responding while somebody's
model set lands.

`IntakeBudget` is that allowance, and three properties are load-bearing:

- **Bytes, not a count.** The cost is the geometry. Forty icons and one
  character model are not the same frame, and any count is wrong for one of
  them.
- **Refused is deferred, never dropped.** The caller puts the arrival back in
  its pending list, which is already where a request that has not finished
  waits - so nothing had to learn a new state.
- **The first arrival of a frame is always admitted.** An asset larger than the
  whole allowance would otherwise be checked and deferred for ever while the
  thing it belongs to stays invisible. One long frame beats never.

**It lives here rather than in the two loops that use it.** The studio's intake
and the client's are both inside a frame loop no test can reach; the same rule
written twice in two untestable places is a rule the build does not check, which
rule 6 has a name for.

## Not here

- **What is inside an asset.** This module delivers and verifies; a mesh format,
  a texture format and an audio format are the importer's and the subsystem's,
  and `ROADMAP.md` v0.9 carries them. A delivery layer that parsed content would
  be a second place that decides what an asset is.
- **Issuing grants.** That is the server's - `assets::Grant` issues and
  `cdn::Gate` opens. This module carries a token and cannot alter it.
- **A signing key.** Verification only, which is the `shared`-tier half of the
  split `assets/AGENTS.md` records.
