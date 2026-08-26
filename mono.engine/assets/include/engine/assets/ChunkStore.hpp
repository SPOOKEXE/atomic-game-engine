#pragma once

// Where chunks actually live on a disk.
//
// CDN.md §7 listed "the chunk store - how chunks are laid out on disk" as *not
// started*, and `cdn::PayloadSource` was left as the seam so that an undecided
// layout would not get baked into the request path. This decides it, and it
// decides it here rather than in the origin for the reason the manifest is here:
// **a publisher writes this tree and a client reads it, so one implementation
// or the two acquire a dialect.**
//
// The layout:
//
//     <root>/manifest.acm       the signature and the manifest
//     <root>/dictionary.zdict   the trained Zstd dictionary, if there is one
//     <root>/chunks/ab/abcd...  one file per chunk, named by its own hash
//
// **Two hex characters of fan-out**, because a hundred thousand files in one
// directory is slow to enumerate on every filesystem and pathological on some,
// and the fan-out costs one `mkdir`.
//
// **Chunks are stored uncompressed and that is not an oversight.** Dedup and
// patching work on chunk boundaries, and a compressed chunk store would trade
// the property content addressing exists for against a ratio the delivery group
// already gets. Two levels, two jobs: chunks are storage, groups are delivery -
// `assets/AGENTS.md` and CDN.md §5.
//
// **Every read verifies.** A chunk's name *is* the hash of its bytes, so
// checking costs one BLAKE3 pass and catches a corrupt disk, a partial write
// and a tampered store with the same check. A store that trusted its own
// filenames would turn a flipped bit into content that fails verification much
// later, at the asset root, with nothing saying which chunk was wrong.
//
// @tier L8 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace engine::assets {

	// A directory of content-addressed chunks.
	//
	// **One owner, one thread** for writing. Reads take no lock and no shared
	// state, so a mounted store may be read from several threads at once - the
	// filesystem is doing the work and the object holds only a path.
	//
	// @since v0.9
	class ChunkStore {
	  public:
		// The manifest's filename inside a store.
		static constexpr const char *MANIFEST_FILE = "manifest.acm";

		// The dictionary's filename inside a store.
		static constexpr const char *DICTIONARY_FILE = "dictionary.zdict";

		// Opens a store, creating the directory when asked.
		//
		// @param directory The store's root.
		// @param create Whether to create it. A publisher passes true; a reader
		//        passes false, so pointing a client at a path that is not there
		//        fails at start-up rather than silently creating an empty store
		//        and then reporting every asset as missing.
		// @return The store, or nothing.
		static std::optional<ChunkStore> Open(const std::filesystem::path &directory, bool create = false);

		// The store's root.
		const std::filesystem::path &Directory() const {
			return Base;
		}

		// Reads one chunk, verifying it against its own address.
		//
		// @param hash The chunk's address.
		// @return The bytes, or nothing when it is absent or does not verify.
		std::optional<std::vector<std::byte>> Read(const ContentHash &hash) const;

		// Writes one chunk.
		//
		// Writing a chunk that is already there is a no-op that answers true:
		// two assets sharing a chunk is the whole point of content addressing,
		// and a publisher should not have to check first.
		//
		// @param hash The address the bytes must hash to.
		// @param bytes The chunk.
		// @return Whether the chunk is now in the store. False for a hash
		//         mismatch, which is a caller bug rather than an IO failure.
		bool Write(const ContentHash &hash, std::span<const std::byte> bytes);

		// Whether a chunk is present, without reading or verifying it.
		//
		// @param hash The chunk's address.
		// @return Whether a file is there.
		bool Contains(const ContentHash &hash) const;

		// Reassembles one asset from its chunks.
		//
		// The result is verified against the asset's root, so a caller gets
		// either the asset or nothing - never a run of chunks that individually
		// verified and together are not the asset.
		//
		// **Every byte is hashed once, not twice.** `Read` already checks each
		// chunk against its own name on the way past, so what is left is the
		// tree over those names - `VerifyAssetShape`, which is the half of
		// `VerifyAsset` that touches no content. Hashing the concatenation again
		// was a second BLAKE3 pass over the whole asset for an answer the reads
		// had produced already.
		//
		// @param asset The manifest's entry for it.
		// @return The bytes, or nothing when any chunk is missing or the
		//         assembled whole does not match the root.
		std::optional<std::vector<std::byte>> ReadAsset(const AssetEntry &asset) const;

		// Reassembles a whole bundle's payload.
		//
		// **This is the definition of a group's bytes**, and it is the one
		// implementation of it: member assets concatenated in
		// `BundleEntry::Assets` order, which is what `Manifest::SliceOf` cuts
		// back up. The origin compresses what this produces and the client
		// splits what it decompresses, so the two ends cannot disagree about
		// where an asset starts.
		//
		// @param manifest The manifest describing the bundle.
		// @param bundle The bundle to assemble.
		// @return The payload, or nothing when any member is unavailable.
		std::optional<std::vector<std::byte>>
		ReadBundle(const Manifest &manifest, const BundleEntry &bundle) const;

		// Reads the manifest and the signature that was published with it.
		//
		// The file is the 64-byte signature followed by the manifest bytes -
		// **the signature first**, so a reader knows what to verify before it
		// has parsed anything. A reader that parsed first and checked after
		// would be running a parser over unverified bytes, which is the
		// arrangement `Grant::Open` exists to avoid at its own layer.
		//
		// This does **not** verify: the key to verify against is the caller's,
		// and a store cannot know it. `Signature.hpp` is the other half.
		//
		// @param[out] signature Filled with the published signature.
		// @return The manifest, or nothing when the file is absent or is not a
		//         manifest.
		std::optional<Manifest> ReadManifest(SignatureBytes &signature) const;

		// Writes the manifest and its signature.
		//
		// @param manifest What to publish.
		// @param signature The signature over its root.
		// @return Whether it was written.
		bool WriteManifest(const Manifest &manifest, const SignatureBytes &signature);

		// Reads the trained dictionary, if this store has one.
		//
		// @return The bytes, or nothing.
		std::optional<std::vector<std::byte>> ReadDictionary() const;

		// Writes the trained dictionary.
		//
		// @param bytes The dictionary.
		// @return Whether it was written.
		bool WriteDictionary(std::span<const std::byte> bytes);

		// How many chunks are stored.
		size_t Count() const;

		// How many bytes the chunks occupy.
		uint64_t Bytes() const;

	  private:
		explicit ChunkStore(std::filesystem::path directory);

		std::filesystem::path PathOf(const ContentHash &hash) const;

		std::filesystem::path Base;
	};
}
