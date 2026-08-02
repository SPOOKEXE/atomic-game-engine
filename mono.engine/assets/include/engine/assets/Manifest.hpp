#pragma once

// What a published set of content is, written down.
//
// The manifest is the one place a *name* becomes a *hash*. A game author writes
// `meshes/rock.mesh`; the manifest says which content that was when this build
// was published, and everything downstream — request, verification, cache key,
// patch — carries the hash and never the name. CDN.md §1: a path reaching the
// request layer is a bug, because a path can be walked and a hash cannot.
//
// Four levels, and this file holds the top three:
//
//     manifest root      BLAKE3 tree over its bundle roots — the signed value
//       bundle root      BLAKE3 tree over its asset roots  — the delivery unit
//         asset root     BLAKE3 tree over its chunk hashes — what a name means
//           chunk        BLAKE3 of the bytes               — Chunker.hpp
//
// **Every list has one canonical order and the bytes are stable.** Two builds
// of the same content produce the same manifest, byte for byte, so it can be
// diffed and cached — the discipline v0.2 already applied to snapshots, here for
// the same reason. A manifest that differs run to run turns "did the content
// change?" into a question nobody can answer cheaply.
//
// This module is the *format*. Deciding which assets belong in which bundle is
// delivery policy and lives with the origin — CDN.md §5.
//
// @tier L8 · shared

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

		// The hash tree root over Chunks, in order.
		ContentHash Root;

		// The chunks, in **stream order**. Not sorted: an offset is what an
		// order means here, and reordering them would change the asset.
		std::vector<ChunkEntry> Chunks;

		// The asset's length — the sum of its chunks.
		uint64_t TotalBytes = 0;
	};

	// A delivery group: the unit that is compressed, streamed and cancelled.
	//
	// What goes in one is policy rather than format, and it lives with the
	// origin — a group has to be independently useful, which is a statement
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
		// unknown — a reader that guesses at a version it does not know is a
		// reader that mis-parses attacker-controlled bytes.
		static constexpr uint16_t VERSION = 1;

		// Adds an asset built from its chunks.
		//
		// Computes the root and the total. Adding a name that is already
		// present replaces it, because publishing twice from one build should
		// not silently produce two rows for one name.
		//
		// @param name The name a game author writes.
		// @param chunks Its chunks, in stream order.
		// @return The asset's root.
		ContentHash AddAsset(std::string name, std::vector<ChunkEntry> chunks);

		// Groups assets into one delivery bundle.
		//
		// Sorts the roots, computes the bundle root and totals the bytes. Roots
		// naming no known asset are refused rather than carried — a bundle
		// pointing at content this manifest does not describe is unfetchable,
		// and finding that out at delivery time is finding out too late.
		//
		// @param assetRoots The assets to group.
		// @return The bundle's root, or nothing if any root is unknown or the
		//         list is empty.
		std::optional<ContentHash> AddBundle(std::span<const ContentHash> assetRoots);

		// The manifest root: the tree over every bundle root, in order.
		//
		// **The one value that is signed.** Everything below it is bound by
		// hash, so this is the only thing a client has to be told
		// authentically. Signature.hpp.
		ContentHash Root() const;

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
		// @param root The asset root.
		// @return The entry, or nullptr.
		const AssetEntry *FindByRoot(const ContentHash &root) const;

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
		// asset are each a refusal — not a warning, and not a partly built
		// object a caller might use.
		//
		// @param reader The bytes to parse.
		// @return The manifest, or nothing. Nothing means refuse the content.
		static std::optional<Manifest> Read(core::ByteReader &reader);

	  private:
		// Sorted by Name, so the serialisation has one arrangement.
		std::vector<AssetEntry> AssetsByName;

		// Sorted by Root, for the same reason.
		std::vector<BundleEntry> BundlesByRoot;
	};
}
