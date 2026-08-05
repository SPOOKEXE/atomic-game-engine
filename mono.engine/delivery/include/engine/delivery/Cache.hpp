#pragma once

// Content this client has already fetched and verified, kept between runs.
//
// **This is what "local cache first" means**, and it is the difference between
// a game that starts instantly the second time and one that re-downloads its
// world every launch. The order in `DeliverySettings::Sources` decides which
// *origin* is tried first; this is consulted before any of them.
//
// **Addressed by asset root, and by nothing else.** A cache keyed on a name
// would go stale the moment content changed under that name, and would need an
// invalidation mechanism — which is precisely the subsystem CDN.md §2 says
// falls out of the hash tree instead of being built. A content address cannot
// go stale: different content is a different key, so a new build populates new
// entries and the old ones age out.
//
// **Bytes are re-verified on read, and that is deliberate even though it costs
// a hash.** A cache lives in a directory a user, another program or a failing
// disk can write to, so trusting it is trusting the least protected thing in
// the delivery path. The alternative — verify on write only — means a single
// flipped bit becomes permanently cached corruption that reproduces on one
// machine and nowhere else, which is among the worst bugs to be handed. BLAKE3
// runs at gigabytes a second; the disk read this is paired with does not.
//
// **Every call takes the manifest's entry rather than a bare hash**, and that
// is not convenience. An asset root is a tree over chunk hashes, so verifying
// bytes against it needs the chunk list — `assets::VerifyAsset`. A cache that
// took a hash alone could only compare `Hasher::Of(bytes)` against the root,
// which is wrong for every asset cut into more than one chunk and right by
// coincidence for some that were not. Taking the entry makes the correct check
// the only one that can be written.
//
// @tier L11 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace engine::delivery {

	// A directory of verified content, addressed by asset root.
	//
	// **One owner, one thread.** A delivery client owns one and reads it from
	// the thread that pumps.
	//
	// @since v0.9
	class ContentCache {
	  public:
		// Opens or creates a cache directory.
		//
		// @param directory Where to keep content. Created if missing.
		// @param capacityBytes What it may hold before evicting.
		// @return The cache, or nothing when the directory could not be made —
		//         which is an ordinary outcome on a read-only install, and a
		//         client runs without a cache rather than refusing to start.
		static std::optional<ContentCache>
		Open(const std::filesystem::path &directory, uint64_t capacityBytes);

		// The directory in use.
		const std::filesystem::path &Directory() const {
			return Base;
		}

		// Reads an asset, verifying it against the manifest's description of it.
		//
		// A file that does not verify is **deleted** rather than returned or
		// left alone. Leaving it would make every later run pay the same failed
		// read; returning it would defeat the point of addressing content by
		// hash.
		//
		// @param asset The manifest's entry, which carries both the address to
		//        look under and the chunk list to check against.
		// @return The bytes, or nothing on a miss or a failed verification.
		std::optional<std::vector<std::byte>> Find(const assets::AssetEntry &asset);

		// Writes an asset, refusing bytes that are not what they claim to be.
		//
		// @param asset The manifest's entry.
		// @param bytes The content.
		// @return Whether it was stored. False for a verification failure —
		//         which is a caller bug rather than a cache failure, since
		//         everything reaching here should already have been verified
		//         against the manifest.
		bool Store(const assets::AssetEntry &asset, std::span<const std::byte> bytes);

		// Whether an asset is present, without reading or verifying it.
		//
		// For a diagnostic or a progress readout. Deciding whether to fetch
		// from this would be a check-then-act race with eviction — call `Find`.
		//
		// @param root The asset root.
		// @return Whether a file is there.
		bool Contains(const assets::ContentHash &root) const;

		// How many bytes are held.
		uint64_t Bytes() const;

		// How many assets are held.
		size_t Count() const;

		// The capacity in use.
		uint64_t Capacity() const {
			return Ceiling;
		}

		// Discards everything.
		void Clear();

	  private:
		explicit ContentCache(std::filesystem::path directory, uint64_t capacityBytes);

		// Where an asset's bytes live.
		//
		// Two hex characters of the hash as a subdirectory, then the full hash.
		// Not decoration: a hundred thousand files in one directory is slow to
		// enumerate on every filesystem and pathological on some, and the
		// fan-out costs one `mkdir`.
		std::filesystem::path PathOf(const assets::ContentHash &root) const;

		// Evicts least-recently-used entries until `incoming` would fit.
		void MakeRoom(uint64_t incoming);

		std::filesystem::path Base;
		uint64_t Ceiling = 0;
	};
}
