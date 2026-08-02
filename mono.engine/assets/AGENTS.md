# assets — module invariants

L8 `content`, `shared` tier. Content addressing, chunking, the hash tree, the
manifest and the one signature over it. `CDN.md` is the design; this file is
what a reviewer should refuse.

## This module does not depend on `scene`

Nor on `ecs`, `world`, `script` or anything above L8. `repo_layout.md` §5.2 puts
that in one sentence: *`assets` does not depend on `scene`, which is why the
delivery service can link it alone.*

That is the entire reason `mono.cdn` can be a program with no simulation in it.
An edge from here to a simulation type would not fail the tier check — both are
`shared` — so this one is a convention the build cannot catch, which by rule 6
means it has to be written down. It is written down here.

## The format is frozen, and three things in particular

Changing any of these rehashes or re-cuts content that already exists
everywhere. None of them is a tuning knob.

- **The hash is BLAKE3-256.** `ContentHash` carries no algorithm tag on purpose:
  two ways to address content is the one place a second mechanism must not be
  allowed to accumulate callers.
- **The gear table and its seed.** `Chunker.cpp` generates 256 constants from
  `GEAR_SEED`. Change either and every chunk boundary in the world moves — every
  stored chunk, every manifest, every client cache, at once.
- **The tag bytes.** `0x01` interior, `0x02` root, `0x03` signing. They are
  domain separation, not decoration; see below.

The chunk *sizes* are not in that list. They are per-`ChunkLimits` and a
manifest records what it was cut with, so a future default is a re-cut of new
content rather than a format break.

## Both checks in `HashTree` are load-bearing

- The **interior tag** stops a subtree root being presented as a chunk hash.
  Without it a verifier combines the same bytes the same way and agrees.
- The **leaf count sealed into the root** pins the tree's shape. Odd levels
  promote their last node unchanged, which is the usual answer and the usual
  hole: without the count, a subtree can be passed off as a whole tree.

Leaves carry no tag, and that is deliberate rather than an oversight. A chunk's
address must be the hash of its content and nothing else, or two systems that
agree on the bytes disagree on the name and dedup stops working across them.

Do not remove one because the other "already covers it". They cover different
things, and the suite names which is which.

## Signing is one signature, at the top

The manifest root is signed. **Nothing below it is.** Everything under the root
is bound by hash, so a client that trusts the root and verifies the chain has
verified the content.

Do not add a per-asset or per-request signature. It would put an asymmetric
operation on the hot path and make an origin's throughput a function of a crypto
primitive, and it would buy nothing that the tree does not already give.

**What is signed is a domain-separated message, never the bare root.** A key
that signs manifest roots may one day sign a grant or a hotpatch, and a signature
over 32 opaque bytes is replayable between them if nothing in the signed material
says which it was.

### The signing/verifying split is a convention

`DATATYPES_LIBRARIES.md` puts Ed25519 verification at `shared` and signing at
`server` and CLI. Both halves are in this one `shared` library, so **the build
cannot enforce that split** — rule 6 again.

The convention: `SigningKey` is called by studio and the CLI. A client, a server
tick or `mono.cdn` calling it is a review failure. The origin holds no signing
key, and that is what makes it safe to deploy on hardware nobody here owns.

## A grant is checked in one order, and the MAC is first

`Grant::Open` verifies the MAC, *then* the expiry, *then* the scope. Nothing
above the MAC check is acted on — the fields are read into a local and no
decision is taken from them.

Reordering this is the mistake worth refusing. Rejecting on an unverified expiry
leaks, by timing and by which counter moved, what an attacker's forged token
contained, and turns the parser into the attack surface the MAC was meant to
remove.

The comparison is `VerifyBufsEqual`, not `==` or `memcmp`. A MAC compared with
an early-out is a MAC an attacker can walk a byte at a time.

**A grant names content hashes and never a path.** There is no field for one and
there must not be: a path has to be re-checked against traversal rules at the
request layer, and a hash is self-limiting because there is nothing to walk.
`ContentRoot` in `mono.cdn` does traversal checking properly and is the *build*
side, not the request path.

**An empty bundle list is refused rather than read as "everything".** A grant
that permits nothing and one that permits all of it must never be the same
value; that mistake is only ever discovered by somebody receiving content they
should not have.

**`nowSeconds` is passed in, never read from a clock here.** This module holds no
notion of "now" to drift out of step with the server's, and expiry stays
testable.

Forged, expired and out-of-scope are counted apart. An expired grant is an
ordinary event — a session that ran long — and a forged one is an alarm. One
counter for both buries the alarm in the noise.

## Every field of a parsed manifest is hostile

A manifest arrives from an origin, and `repo_layout.md` §1 says anyone can run
one. `Manifest::Read` refuses — returning nothing and marking the reader failed
— on a wrong magic, an unknown version, an out-of-range count, a name that is
empty or over-long, a byte total that disagrees with its chunks, a root that
does not match what it claims to cover, a list out of canonical order, or a
bundle naming an asset the manifest does not describe.

**It recomputes roots rather than believing them.** Taking a written root on
trust would let a manifest name content whose chunks are something else, which
is the whole property the tree exists to provide.

Never return a partly built `Manifest`. A half-parsed one is the shape a caller
uses by accident.

## Canonical order, and byte-stable output

Assets sorted by name, bundle members sorted by hash, bundles sorted by root,
chunks in stream order because an offset is what an order means there.

Two builds of one set of content produce identical manifest bytes. This is the
discipline v0.2 applied to snapshots and it is here for the same reason: a
manifest that differs run to run cannot be diffed, cannot be cached, and turns
"did the content change?" into a question nobody can answer cheaply.

`AddAsset` and `AddBundle` insert in sorted position rather than appending and
sorting later, so there is no window in which the object is in an order the
serialiser would not produce.

## No vendor type in a public header

`Vendor::blake3` and `Vendor::cryptopp` are both `VENDOR`, never
`VENDOR_PUBLIC`. `Hasher` sizes its own inline storage and asserts against the
real struct in the source, which is why `blake3.h` reaches nothing.

The alternative — a `unique_ptr` pimpl — would put an allocation on the path that
hashes every chunk of every asset. Do not "simplify" it back.

## What is policy and lives elsewhere

**Which assets go in which bundle** is delivery policy, not format. A group has
to be independently useful, which is a statement about the game rather than about
bytes — `CDN.md` §5, and it belongs with the origin.

**Where a seed comes from.** `SigningKey::FromSeed` takes 32 bytes and asks no
questions. `core::Random` is not a cryptographic generator and must never be used
for one.

**Compression.** Chunks are stored uncompressed so that dedup works on them;
compression is per delivery group and in flight. Do not compress here.

## Not here yet

- Fetching. There is no network in this module and there will not be — transport
  is `Engine::net` at L11, and this stays the format.
- Containers and cooked-asset layout: what is *inside* an asset, as opposed to
  how it is named and delivered.
- GUIDs, and the mapping from an authoring identity to a name.
- Grants. They are the origin's, and they name content hashes from here.

`ROADMAP.md` v0.8 carries the rest.
