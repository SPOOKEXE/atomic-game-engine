#pragma once

// Published content manifest. Names map to hashed assets; canonical ordering
// keeps serialized bytes stable. Bundle selection is origin policy.
// @tier L8 · shared

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

	// One chunk of one asset.
	struct ChunkEntry {
		// The chunk's address: BLAKE3 over its bytes, and nothing else.
		ContentHash Hash;

		// Its length. Held here so a client can size a buffer and check the
		// arrival before hashing it, rather than after.
		uint32_t Bytes = 0;
	};

	// One asset: what a name meant when this manifest was published.
	struct AssetEntry {
		// The name a game author wrote. The only string in the format that
		// crosses a boundary, and the reason AGENTS.md rule 4 exists.
		std::string Name;

		// What subsystem this belongs to, decided once at publish time.
		//
		// Recorded rather than derived downstream: deriving it from the name at
		// each reader is two opinions about what `rock.glb` is, which disagree
		// the day one reader learns an extension the other has not.
		// AssetKind.hpp carries the argument.
		AssetKind Kind = AssetKind::Unknown;

		// The hash tree root over Chunks, in order.
		//
		// **Over the chunks and nothing else**, which is what makes it an
		// address: two builds that produce the same bytes produce the same root
		// whatever the file was called, so dedup works across names and across
		// versions. The name and the kind are bound to it a level up - see
		// `Manifest::Root`.
		ContentHash Root;

		// The chunks, in **stream order**. Not sorted: an offset is what an
		// order means here, and reordering them would change the asset.
		std::vector<ChunkEntry> Chunks;

		// The asset's length - the sum of its chunks.
		uint64_t TotalBytes = 0;
	};

	// A delivery group: the unit that is compressed, streamed and cancelled.
	//
	// What goes in one is policy rather than format, and it lives with the
	// origin - a group has to be independently useful, which is a statement
	// about the game rather than about bytes. CDN.md §5.
	struct BundleEntry {
		// The hash tree root over Assets, in order.
		ContentHash Root;

		// Member asset roots, **sorted**. A bundle is a set, so its identity
		// must not depend on the order somebody happened to add them in.
		std::vector<ContentHash> Assets;

		// The group's uncompressed length, for sizing and for priority.
		uint64_t TotalBytes = 0;
	};

	// Verifies bytes against the asset's chunk tree.
	//
	// @param asset The manifest's entry, carrying the chunk list and the root.
	// @param bytes The candidate content.
	// @return Whether the bytes are that asset.
	// @since v0.9
	bool VerifyAsset(const AssetEntry &asset, std::span<const std::byte> bytes);

	// The half of `VerifyAsset` that needs no content.
	//
	// **Not a second way to verify an asset.** `VerifyAsset` is two questions
	// asked together: are these bytes the chunks the entry lists, and do those
	// chunks add up to the asset the entry claims. The first needs the content
	// and costs a BLAKE3 pass over all of it; the second needs only the entry
	// and costs one compression per chunk. This is the second, and `VerifyAsset`
	// is implemented as the first followed by a call to this, so there is one
	// definition of each rather than two that agree until they do not.
	//
	// It exists because one caller has already asked the first question a
	// better way. `ChunkStore::ReadAsset` reads each chunk through
	// `ChunkStore::Read`, which hashes it against its own name - the same
	// comparison, a chunk at a time, and able to say *which* chunk was wrong.
	// Handing the concatenation to `VerifyAsset` afterwards hashed every byte a
	// second time for an answer already known.
	//
	// **A caller that has not checked the content must not call this.** It says
	// nothing whatever about any bytes; on its own it is satisfied by an entry
	// that describes content nobody has looked at.
	//
	// @param asset The manifest's entry.
	// @return Whether the chunk list totals what the entry claims and roots to
	//         what the entry is addressed by.
	// @since v0.19
	bool VerifyAssetShape(const AssetEntry &asset);

	// Where one asset's bytes sit inside its bundle's payload.
	//
	// @since v0.9
	struct BundleSlice {
		// Bytes from the start of the payload.
		uint64_t Offset = 0;

		// How many bytes belong to this asset.
		uint64_t Bytes = 0;
	};

	// The published set of content, and the value that gets signed.
	//
	// Build one by adding assets and then bundling them; read one by parsing
	// bytes. The two paths produce the same structure, which is what keeps the
	// origin and the running game on one implementation of the format.
	class Manifest {
	  public:
		// The format's magic, so a wrong file fails at its first four bytes
		// rather than somewhere confusing.
		static constexpr uint32_t MAGIC = 0x314D4341; // "ACM1"

		// The format version. Bumped when the layout changes, and refused when
		// unknown - a reader that guesses at a version it does not know is a
		// reader that mis-parses attacker-controlled bytes.
		//
		// **2 since v0.9**, which added the asset kind and folded the
		// descriptor table into the signed root. Nothing had yet published a
		// version-1 manifest to disk, so the bump costs nobody a re-publish -
		// which is exactly why it was worth doing now rather than after there
		// was content in the world addressed under the old one.
		static constexpr uint16_t VERSION = 2;

		// Adds an asset built from its chunks.
		//
		// Computes the root and the total. Adding a name that is already
		// present replaces it, because publishing twice from one build should
		// not silently produce two rows for one name.
		//
		// @param name The name a game author writes.
		// @param kind What subsystem it belongs to. `KindOfName` is how a
		//        publisher decides, and it is the only place that decides.
		// @param chunks Its chunks, in stream order.
		// @return The asset's root.
		ContentHash AddAsset(std::string name, AssetKind kind, std::vector<ChunkEntry> chunks);

		// Groups assets into one delivery bundle.
		//
		// Sorts the roots, computes the bundle root and totals the bytes. Roots
		// naming no known asset are refused rather than carried - a bundle
		// pointing at content this manifest does not describe is unfetchable,
		// and finding that out at delivery time is finding out too late.
		//
		// @param assetRoots The assets to group.
		// @return The bundle's root, or nothing if any root is unknown or the
		//         list is empty.
		std::optional<ContentHash> AddBundle(std::span<const ContentHash> assetRoots);

		// The manifest root: the one value that is signed.
		//
		// A two-leaf tree over the **descriptor root** and the **bundle root**,
		// and it is two rather than one because of a gap this closes.
		//
		// Until v0.9 the root covered bundle roots alone, which bound every
		// byte of content and none of what a byte was *called*. An origin
		// serving a manifest with two names swapped would hand a client content
		// that verified perfectly against the signed root and was the wrong
		// asset - and the same would now be true of the kind, which is what a
		// client routes on. Signing the content but not the index is signing
		// the half nobody looks up by.
		//
		// So the descriptor root is a tree over one hash per asset, covering
		// its name, its kind and its content root together. A client that
		// trusts this value has verified what the content is, what it is
		// called, and what it is for.
		//
		// The asset root itself is untouched and still covers chunks alone,
		// because that is what makes it an address - folding a name into it
		// would give two identical files under two names two different roots
		// and lose dedup across them.
		ContentHash Root() const;

		// The tree over every asset's name, kind and content root.
		//
		// Exposed because a client verifying a manifest it received recomputes
		// this rather than believing it, and doing that needs the same
		// definition the publisher used. One implementation, one definition.
		ContentHash DescriptorRoot() const;

		// The tree over every bundle root, in order.
		//
		// The delivery half of what `Root` covers.
		ContentHash BundleRoot() const;

		// Every asset, sorted by name.
		const std::vector<AssetEntry> &Assets() const {
			return AssetsByName;
		}

		// Every bundle, sorted by root.
		const std::vector<BundleEntry> &Bundles() const {
			return BundlesByRoot;
		}

		// The asset a name resolves to.
		//
		// The lookup that turns authoring into delivery, and the last place a
		// name is used at all.
		//
		// @param name The name to resolve.
		// @return The entry, or nullptr when the manifest does not describe it.
		const AssetEntry *Find(std::string_view name) const;

		// The asset with this root.
		//
		// A binary search through `RootOrder`. It was a scan until v0.19, on
		// the argument that it ran while a manifest was being built rather than
		// while one was being served - which stopped being true when
		// `delivery::Client::Split` began calling it per member of every
		// arriving group, inside `Pump`, at the tick barrier.
		//
		// @param root The asset root.
		// @return The entry, or nullptr. The first in name order when two names
		//         carry identical content, which is the answer the scan gave.
		const AssetEntry *FindByRoot(const ContentHash &root) const;

		// The bundle with this root.
		//
		// Bundles are held in root order, so this is a binary search. It is here
		// rather than at its two callers - a delivery job holds a bundle root
		// and a relay is asked for one by route - because both of them walked
		// the list instead, which is the format's own knowledge of its own
		// arrangement written out twice somewhere else.
		//
		// @param root The bundle root.
		// @return The bundle, or nullptr.
		// @since v0.19
		const BundleEntry *FindBundle(const ContentHash &root) const;

		// The bundle a given asset is delivered in.
		//
		// The lookup a fetch actually needs: a client wants one asset, and the
		// unit that travels is the group it belongs to. Answering it here
		// rather than in the delivery client keeps the format's own knowledge
		// of its own structure in one place.
		//
		// @param assetRoot The asset.
		// @return The bundle, or nullptr when no bundle carries it - which is a
		//         publisher's mistake and is worth being able to detect.
		const BundleEntry *BundleFor(const ContentHash &assetRoot) const;

		// Where an asset sits inside a bundle's uncompressed payload.
		//
		// **This is the group's internal layout, and it is defined by the
		// manifest rather than written into the payload.** A bundle's bytes are
		// its member assets concatenated in the order `BundleEntry::Assets`
		// holds them - sorted by root - with no framing, no header and no
		// index between them.
		//
		// Deriving the layout instead of transmitting it is what makes it
		// trustworthy: the offsets come from data the manifest root already
		// signs, so an origin cannot move an asset's boundary without breaking
		// the signature. A payload carrying its own index would be an origin
		// telling a client where to cut, which is one more thing a compromised
		// origin gets to decide.
		//
		// It also costs nothing on the wire, which is the smaller reason.
		//
		// CDN.md §7 listed the on-disk chunk layout as undecided; this settles
		// the *group* layout, which is the half delivery needs.
		//
		// @param bundle The bundle whose payload is being cut up.
		// @param assetRoot The asset to locate.
		// @return Where it sits, or nothing when the bundle does not carry it.
		std::optional<BundleSlice> SliceOf(const BundleEntry &bundle, const ContentHash &assetRoot) const;

		// Every asset of one kind, in name order.
		//
		// What "connect and get assets of types" resolves to at the format
		// layer. Returns pointers into this manifest, so they live exactly as
		// long as it does.
		//
		// @param kind The kind to select.
		// @return The matching entries.
		std::vector<const AssetEntry *> OfKind(AssetKind kind) const;

		// Writes the manifest.
		//
		// Byte-stable: the same content produces the same bytes on any machine
		// and in any build. That is a requirement of the format rather than a
		// property of this implementation, and the suite pins it.
		//
		// @param writer Where the bytes go.
		void Write(core::ByteWriter &writer) const;

		// Parses what Write produced.
		//
		// **Every field is hostile.** A manifest arrives from an origin, and
		// §1 of repo_layout.md says anyone can run one. A wrong magic, an
		// unknown version, a count that does not match what follows, a chunk
		// total that disagrees with its chunks, or a bundle naming an unknown
		// asset are each a refusal - not a warning, and not a partly built
		// object a caller might use.
		//
		// @param reader The bytes to parse.
		// @return The manifest, or nothing. Nothing means refuse the content.
		static std::optional<Manifest> Read(core::ByteReader &reader);

	  private:
		// Where `AssetsByName[position]` belongs in `RootOrder`.
		std::vector<uint32_t>::const_iterator IndexSlotOf(size_t position) const;

		// Puts `AssetsByName[position]` into `RootOrder`, shifting nothing.
		void IndexPlace(size_t position);

		// Puts `AssetsByName[position]` into `RootOrder`, having just inserted
		// it there. Shifts the positions the insert moved along, which is what
		// makes it wrong for a replacement.
		void IndexInserted(size_t position);

		// Takes `AssetsByName[position]` out of `RootOrder`, before it is
		// overwritten. The entry must still be the one the index was built for.
		void IndexReplaced(size_t position);

		// Builds `RootOrder` from scratch, for the parse path that fills
		// `AssetsByName` in one go.
		void IndexAll();

		// The first entry in `RootOrder` whose root is not below `root`.
		std::vector<uint32_t>::const_iterator LowerBoundByRoot(const ContentHash &root) const;

		// Sorted by Name, so the serialisation has one arrangement.
		std::vector<AssetEntry> AssetsByName;

		// Sorted by Root, for the same reason.
		std::vector<BundleEntry> BundlesByRoot;

		// Positions into `AssetsByName`, ordered by the root each names and
		// then by the position itself.
		//
		// **Derived, and derived from one place.** `AssetsByName` is the only
		// statement of what this manifest describes; this holds no roots of its
		// own, only where to find them, so the two cannot say different things
		// about an asset. Every operation that changes `AssetsByName` updates
		// this in the same call, and there is no path that appends an asset
		// without one - which is what makes the "second thing to keep true"
		// objection the old scan was defended with not apply.
		//
		// **Positions and not pointers**, so a copy of a manifest copies a
		// working index and a reallocation of `AssetsByName` does not invalidate
		// it.
		//
		// **Ties break towards the earlier asset.** Two names can carry
		// identical content and so share a root - that is what dedup is - and
		// the scan this replaced returned the first of them in name order. The
		// position is the tiebreaker precisely so that it still does.
		//
		// @since v0.19
		std::vector<uint32_t> RootOrder;
	};
}
