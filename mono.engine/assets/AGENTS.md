# assets - module invariants

L8 `content`, `shared` tier. Content addressing, chunking, the hash tree, the
manifest and the one signature over it. `CDN.md` is the design; this file is
what a reviewer should refuse.

## This module does not depend on `scene`

Nor on `ecs`, `world`, `script` or anything above L8. `repo_layout.md` §5.2 puts
that in one sentence: *`assets` does not depend on `scene`, which is why the
delivery service can link it alone.*

That is the entire reason `mono.cdn` can be a program with no simulation in it.
An edge from here to a simulation type would not fail the tier check - both are
`shared` - so this one is a convention the build cannot catch, which by rule 6
means it has to be written down. It is written down here.

## The format is frozen, and three things in particular

Changing any of these rehashes or re-cuts content that already exists
everywhere. None of them is a tuning knob.

- **The hash is BLAKE3-256.** `ContentHash` carries no algorithm tag on purpose:
  two ways to address content is the one place a second mechanism must not be
  allowed to accumulate callers.
- **The gear table and its seed.** `Chunker.cpp` generates 256 constants from
  `GEAR_SEED`. Change either and every chunk boundary in the world moves - every
  stored chunk, every manifest, every client cache, at once.
- **The tag bytes.** `0x01` interior, `0x02` root, `0x03` signing, `0x04`
  descriptor. They are domain separation, not decoration; see below.

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
cannot enforce that split** - rule 6 again.

The convention: `SigningKey` is called by studio and the CLI. A client, a server
tick or `mono.cdn` calling it is a review failure. The origin holds no signing
key, and that is what makes it safe to deploy on hardware nobody here owns.

## A grant is checked in one order, and the MAC is first

`Grant::Open` verifies the MAC, *then* the expiry, *then* the scope. Nothing
above the MAC check is acted on - the fields are read into a local and no
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
ordinary event - a session that ran long - and a forged one is an alarm. One
counter for both buries the alarm in the noise.

## Every field of a parsed manifest is hostile

A manifest arrives from an origin, and `repo_layout.md` §1 says anyone can run
one. `Manifest::Read` refuses - returning nothing and marking the reader failed
- on a wrong magic, an unknown version, an out-of-range count, a name that is
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

The alternative - a `unique_ptr` pimpl - would put an allocation on the path that
hashes every chunk of every asset. Do not "simplify" it back.

## What is policy and lives elsewhere

**Which assets go in which bundle** is delivery policy, not format. A group has
to be independently useful, which is a statement about the game rather than about
bytes - `CDN.md` §5, and it belongs with the origin.

**Where a seed comes from.** `SigningKey::FromSeed` takes 32 bytes and asks no
questions. `core::Random` is not a cryptographic generator and must never be used
for one.

**Compression.** Chunks are stored uncompressed so that dedup works on them;
compression is per delivery group and in flight. Do not compress here.

## The root covers the index as well as the content

Until v0.9 `Manifest::Root` was a tree over bundle roots alone, which bound every
byte of content and none of what a byte was *called*. An origin serving a
manifest with two names swapped handed a client content that verified perfectly
and was the wrong asset. Signing the content and not the index is signing the
half nobody looks anything up by.

It is now a two-leaf tree over the **descriptor root** and the **bundle root**,
where a descriptor covers an asset's name, its kind and its content root
together. One signature, still at the top; a client that trusts it has verified
what the content is, what it is called and what it is for.

**The asset root is untouched and still covers chunks alone.** That is what makes
it an *address*: folding a name into it would give two identical files under two
names two different roots and lose dedup across them.

A descriptor is length-prefixed and tagged `0x04`. Both for the reasons
`HashTree`'s own tags exist - without the length, `ab` + `c` and `a` + `bc` are
one hash and the binding does not hold.

## `AssetKind` is decided once, by the publisher

A kind says which subsystem a blob belongs to and nothing about what is inside
it. `KindOfName` is the whole of the extension-to-kind opinion in this engine and
has exactly one caller: the publisher. Everything else reads the manifest.

Deriving it at each reader instead would be two opinions about what `rock.glb`
is, which disagree the day one reader learns an extension the other has not.

**The list is closed and its numbers are part of the format.** Appending is safe;
renumbering is not. An unknown kind is read as `Unknown` rather than refused,
because a manifest from a later build is a legitimate document - refusing would
make every kind added later a hard break for every client already deployed.

## Verification is `VerifyAsset` and nothing else

An asset root is a **tree over chunk hashes**, not the digest of its content, so
`Hasher::Of(bytes) == asset.Root` is wrong for every asset cut into more than one
chunk and right by coincidence for some that were not.

That is the worst shape a check can have - it passes in the small case somebody
tests with - and it was written once, in the delivery cache, where it silently
refused every real asset as a cache miss. One implementation now, with three
callers: the chunk store reassembling, the delivery client checking what arrived
from an origin, and the same client checking what came out of its own cache.

**It is two halves and they are named apart, which is not a second way to
verify.** `VerifyAsset` asks whether these bytes are the chunks the entry lists,
which costs a BLAKE3 pass over all of them, and then whether those chunks add up
to the asset the entry claims, which touches no content at all. The second half
is `VerifyAssetShape` and `VerifyAsset` is defined as the first followed by it,
so there is one definition of each rather than two that agree until they do not.

`ChunkStore::ReadAsset` is the one caller that takes the halves apart, because
its reads have already made the first: every chunk is hashed against its own name
on the way past, which is the same comparison and says *which* chunk was wrong.
Handing the concatenation to `VerifyAsset` afterwards hashed every byte a second
time - 14.76 us per chunk against 9.17 us, measured over a 4 MiB asset.

**A caller that has not checked the content must not call `VerifyAssetShape`.**
On its own it says nothing whatever about any bytes.

## The chunk store is the format's, not the origin's

`ChunkStore` decides how chunks are laid out on a disk - the thing `CDN.md` §7
listed as undecided. It is here rather than in `mono.cdn` for the reason the
manifest is: **a publisher writes that tree and a client reads it**, so one
implementation or the two acquire a dialect.

**Chunks are stored uncompressed.** Dedup and patching work on them; compression
is per delivery group and in flight, and belongs to `Engine::delivery`. Two
levels, two jobs.

**Every read verifies.** A chunk's name *is* the hash of its bytes, so checking
costs one pass and catches a corrupt disk, a partial write and a tampered store
with the same check.

`ReadBundle` is the one producer of a group's payload - members concatenated in
`BundleEntry::Assets` order - and `SliceOf` is the one consumer's definition of
where each asset sits. The origin compresses what one produces and the client
splits what the other describes, so the two ends cannot disagree about where an
asset starts.

## The manifest's lookups are all indexed now, and the index holds no facts

`Find` was always a binary search over the name order the format already
requires. `FindByRoot` was a scan, defended by a comment saying a second index
would be a second thing to keep true and that this ran while a manifest was being
built rather than while one was being served. The second half stopped being true:
`delivery::Client::Split` calls it per member of every arriving group, inside
`Pump`, at the tick barrier - 11.96 us to locate one member of a 32-member group
in a 4096-asset manifest, measured.

`RootOrder` answers it, and it answers the objection by **holding positions into
`AssetsByName` rather than a copy of anything**. There is still one statement of
what a manifest describes, so the two cannot disagree about an asset; what the
index adds is an order over it. Every operation that changes `AssetsByName`
updates it in the same call and there is no path that appends an asset without
one. Ties break towards the earlier asset, because two names can carry identical
content and the scan returned the first of them in name order.

`FindBundle` is the same argument one list over, and it removed two open-coded
scans - `delivery::Client` and `delivery::Relay` each held one.

**`BundleFor` is still a walk, and that is a decision.** An asset-to-bundle index
is not positions over a list this manifest already holds in that order; it is a
new key, and the format has no such mapping. The caller that repeated the lookup
was the problem and it was fixed there: `delivery::Client` records the carrier
its `Resolve` already found, instead of asking again per pending request per
pump.

## Not here yet

- Fetching. There is no network in this module and there will not be - transport
  is `Engine::net` at L11 and the fetch path is `Engine::delivery` above it.
- Containers and cooked-asset layout: what is *inside* an asset, as opposed to
  how it is named and delivered.
- GUIDs, and the mapping from an authoring identity to a name.
- Grants. They are the origin's, and they name content hashes from here.

`ROADMAP.md` v0.8 carries the rest.

## The mesh format's bounds are derived, and must stay derived

`Mesh::Write` writes no bounding box and `Mesh::Read` computes one. That is not
an omission to be tidied up later: a stored bound is a second copy of a fact the
vertices already carry, and it is **the one field of a published mesh an
attacker gets to choose**. A mesh claiming a zero box disappears from every
frustum test; one claiming a kilometre draws from everywhere. Both are invisible
in the file and both read as renderer bugs.

Adding the field "so the reader does not have to walk the vertices" would be
buying a pass the reader is already making.

## A texture's mip levels are shaped by rule, never by the file

`TextureData::Mips` holds the levels' bytes and nothing else. Each level's width
and height come from `MipExtent`, and `IsValid` refuses a level that is not
exactly that size - which is the mesh bounding box's argument one format over: a
stored per-level width is a second copy of a fact the base dimensions already
carry, and it is the copy an attacker gets to choose. A level claiming to be
larger than it is is a GPU upload reading past the end of a buffer.

The count is bounded by `MipLevelCount` of the dimensions rather than by a
constant of its own. That bound is tighter and it stays true if
`MAXIMUM_DIMENSION` ever moves.

**The levels are beside the base, not concatenated with it.** `Pixels` means *the
image* to a dozen call sites that check it against `Width * Height *
BytesPerPixel` - `IsValid`, `render::TextureTable::Upload`, `ResizeImage`, the
opaque pass, the studio thumbnailer. One buffer holding the chain would make
every one of them read a third too much while still compiling. Do not "flatten"
it.

## The box filter lives here, and the tier it lives at was the whole bug

`Resample.hpp` - `ResizeImage`, `MipChainLevels`, `BuildMipChain`. It was
`bake::ResizeImage` at L9 until v0.15, and being one tier up meant this module
could not use its own filter: `MakeBuiltin`'s checker, `render::DefaultTexture`
and `render::MissingTexture` are all pixels generated below the importers, so all
three uploaded at one level and shimmered at distance with three modules looking
correct. Moving it down fixed all three at once, and it moved rather than being
copied - a second box filter is how two textures start disagreeing about what a
half-size copy of themselves is.

**It belongs here on its own merits, not only because this needed it.** Every
line of it is a weighted average over bytes `Texture.hpp` defines. It reads no
foreign file, links no vendor and knows nothing about a decoder; `bake` still
owns everything that turns *somebody else's* file into a `TextureData`, and this
owns what happens to one afterwards.

## A flipbook's mip chain stops before its frames bleed

`BuildMipChain` halves a still image all the way to one pixel. **A sheet of
animation frames stops earlier, and the stopping point is not a tuning knob.**
Halving a grid is safe only while every destination pixel still falls inside one
cell; one level past that, a pixel averages two frames and the sheet shows a
ghost of the next frame at distance. That reads as the flipbook's cell arithmetic
being wrong rather than as a chain one level too long, which is why it is refused
rather than approximated.

The chain therefore ends at the last level whose cells are still an exact
halving, which is the largest power of two dividing both cell dimensions and
which lands on the level where a frame is one pixel. A sheet whose cells are odd,
or whose dimensions its grid does not divide, gets **no chain at all** rather
than an approximate one.

The one exception is a 1x1 grid, which gets the full chain: there is no interior
boundary to bleed across. That case is not a curiosity - `bake/Gif.cpp` gives
every single-frame GIF a 1x1 grid, so without it every imported still would lose
its levels to a neighbour that does not exist.

The two alternatives were weighed and both are worse. Padding the cells with
gutters changes what a flipbook *is* - every consumer divides the sheet by
`FlipbookSide`, so a gutter is a change to `TextureData`, to
`render::FlipbookCellAt` and to every UV that samples one. Refusing the texture
outright throws away an image that was perfectly good without a chain.

## A generated built-in builds its own chain, because nothing else will

`MakeBuiltin(BuiltinTexture)` calls `BuildMipChain` before returning, and so do
`render::DefaultTexture` and `render::MissingTexture`. These are the only
textures in the engine that reach a sampler without passing through a bake
graph, so there is no pipeline stage to put a `Mipmap` node in - the generator is
the last place that knows the image is finished. A built-in that returned without
a chain would be the one texture in the engine that still aliased, and the
checker is exactly the sheet an author tiles across a floor and then looks at
from across the map.

## `Submesh` holds strings where the rest of the engine holds names

`Material` and `Texture` are `std::string` and this is the one place the format
departs from rule 4's usual answer. Interning takes a process-wide mutex and
grows a registry nothing empties, and every byte here arrives from an origin
anybody may run - so a mesh naming ten thousand distinct materials would be an
unbounded allocation in a shared table, reachable from content.
`delivery::Asset::Name` is a `std::string` for the same reason.

The consumer interns when it **registers** the mesh, which is the point where
the name has already been accepted. `render::MeshTable::Add` is that point.

## The built-ins are generated, and the suite checks the winding

`MakeBuiltin` produces geometry rather than reading a table, and the reason is
what the suite can then check: every triangle of every built-in points outwards,
and every closed one is a manifold - each edge shared by exactly two triangles
that traverse it in opposite directions. That single property subsumes "no
hole", "no duplicated face", "no face wound backwards relative to its neighbour"
and "no missing pole triangle", none of which a picture would show.

A face wound the wrong way is culled when you look at it and drawn when you
cannot. It shipped exactly once, in the cube, and the check that caught it is
the ancestor of `tests/Builtin.cpp`.

**Everything is a unit shape about its own origin**, spanning -0.5 to +0.5.
`render::Renderer` folds `DrawInstance::HalfExtent` into the model matrix, so a
generator returning a radius-one sphere would make every part twice the size it
says it is - and the mistake would read as a physics bug, because the collider
would still be right.

## There is one extension table, and it answers three questions

`ContentForm.cpp` holds it. A name's extension decides the **form** (the format
- `Png`, `Gif`, `Svg`, `Mp4`), the **kind** (`AssetKind`, the routing label
several forms share) and whether it is a **source** a baker still has to
convert.

Those were two lists until v0.15 - an `EXTENSIONS` table for routing and a
separate `SOURCES` list beside it, with a comment on the second saying it was
the one that must not go stale. Adding a format meant editing both and nothing
noticed when somebody edited one. It is three columns now, `KindOfName` is
`KindOfForm(FormOfName(name))` and `IsRuntimeReadable` is a single negation, so
a row is the whole edit.

**Do not add a second table.** Anything that wants to know what an extension
means asks here, and anything derived from the set of forms - the content flags
are - is generated from `AllForms` rather than listed again.

## A content policy is consulted at the door, before anything decodes

`ContentPolicy` says which forms a deployment will handle, and the rule the
build cannot check is that **every place which decodes, copies or publishes
content by name consults it first**. There are four today: `assetc`'s dispatch,
`cdn::Publisher`'s classification, `client::Client::RequestAsset` and the
studio's picker.

Consulting it *late* passes every obvious test and fails the point of it. The
`cdn` suite asserts the chunk store is **smaller** after a refused publish
rather than only that the manifest is shorter, because a gate applied after the
chunker would satisfy the second and not the first - and the whole reason to
refuse an SVG is that the rasteriser is never reached.

**A refusal is named, counted and never silent.** `PublishReport::Refused` and
`assetc::Report::Refused` are separate from the failure counts, because a
refusal is not a failure and a caller treating any failure as a broken run must
not be broken by a deployment deciding it does not want GIFs.

**Everything is allowed by default and `Unknown` always is.** An origin moves
bytes it does not interpret, so a policy that refused what it cannot name would
refuse the next format before it was added; `content.unknown` is the one flag
that closes the list. A program that never declared the flags gets everything,
which is what this engine did before they existed - a dead flag handle reads
`false`, and defaulting to refusal there would make a tool that forgot to
register a table produce an empty bake with no explanation.
