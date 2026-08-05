#pragma once

// Compressing a delivery group, and only a delivery group.
//
// **Per group. Not per file, and not per manifest.** CDN.md §5 settles the level
// and both alternatives are worse in a specific way: per file loses the
// cross-file redundancy that is most of the ratio on many small assets, and per
// manifest defeats range requests, partial fetch and everything the hash tree
// exists for.
//
// **This fights chunk-level dedup, and the two are kept at different levels
// rather than reconciled.** Chunks are stored uncompressed so that dedup and
// patching work on them; a group is the compressed thing in flight. The origin
// stores chunks and *prepares* groups from them, and a prepared group is cached
// so it is built once and streamed many times.
//
// A dictionary is what makes this worth doing. A game's content is thousands of
// small files sharing a great deal — the same vertex layouts, the same material
// fields, the same strings — and without one they compress as thousands of
// independently mediocre blobs. The dictionary is itself content-addressed and
// shipped as a group, so it versions like everything else instead of becoming an
// out-of-band file that can drift.
//
// **Decompression treats its input as hostile.** A frame arrives from an origin,
// and `repo_layout.md` §1 says anyone can run one. A frame header can claim any
// decompressed size it likes, so nothing here allocates against what the frame
// says — only against what the *manifest* already said the group weighs.
//
// @tier L11 · shared

#include <engine/assets/ContentHash.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::delivery {

	// A trained Zstd dictionary, addressed by its content like everything else.
	//
	// Trained once per published build and then loaded; never trained on a
	// serving path, because that would make an origin's first response wait for
	// a pass over the whole sample.
	class Dictionary {
	  public:
		// What a trained dictionary is allowed to weigh by default.
		//
		// 110 KiB is Zstd's own suggested size and is chosen rather than
		// derived. It ships to every client once and then improves the ratio of
		// every group forever, so it is the cheapest large thing in the format —
		// but it is still a number CDN.md §9 has no measurement behind.
		static constexpr size_t DEFAULT_TRAINED_BYTES = 110 * 1024;

		// Trains a dictionary over a sample of a game's content.
		//
		// Run once per published build, not per request — training is
		// substantial CPU over the whole sample, and putting it on a serving
		// path would make an origin's first response wait for it. Its output is
		// content-addressed and shipped as a group like anything else.
		//
		// Zstd needs a reasonable number of reasonably similar samples and
		// refuses when it has too little to learn from. That refusal is passed
		// through rather than papered over: a dictionary trained on too little
		// is worse than none, because it costs bytes on every fetch and buys
		// nothing.
		//
		// @param samples One entry per sample. Small files from the content
		//        being published, not the whole content.
		// @param capacityBytes The largest dictionary to produce.
		// @return The dictionary, or nothing when Zstd had too little to learn
		//         from.
		static std::optional<Dictionary> Train(
			std::span<const std::span<const std::byte>> samples, size_t capacityBytes = DEFAULT_TRAINED_BYTES
		);

		// Loads a dictionary from its bytes.
		//
		// Refuses anything without a trained dictionary's magic. Zstd would
		// happily accept arbitrary bytes as a "raw content" dictionary, which is
		// legal and nearly useless — and a manifest shipped where a dictionary
		// was expected would then cost ratio on every group for the life of the
		// deployment, silently.
		//
		// @param bytes The trained dictionary.
		// @return The dictionary, or nothing when the bytes are empty or are not
		//         a trained dictionary.
		static std::optional<Dictionary> Load(std::span<const std::byte> bytes);

		// The dictionary's own content hash.
		//
		// Half of a prepared group's cache key: a group compressed against one
		// dictionary is not the same artefact as the same group compressed
		// against another, and caching them under one key would serve bytes a
		// client cannot decode.
		const engine::assets::ContentHash &Hash() const {
			return Address;
		}

		// The raw dictionary bytes, for shipping it as a group of its own.
		std::span<const std::byte> Bytes() const {
			return Raw;
		}

	  private:
		Dictionary() = default;

		std::vector<std::byte> Raw;
		engine::assets::ContentHash Address;
	};

	// Compresses and decompresses group payloads.
	//
	// Stateless with respect to the data. The same bytes, level and dictionary
	// give the same frame, which is what lets a prepared group be cached by
	// content rather than rebuilt per request.
	class GroupCodec {
	  public:
		// The compression level.
		//
		// Chosen rather than derived, and CDN.md §9 carries it as an open
		// question — there is no measurement behind it yet and saying so is
		// better than implying there is. 9 is above Zstd's default of 3 because
		// preparation happens once and streaming happens many times, so the
		// origin's CPU is the cheap side of that trade.
		static constexpr int DEFAULT_LEVEL = 9;

		// Compresses a group payload.
		//
		// @param payload The group's concatenated, uncompressed bytes.
		// @param level The compression level.
		// @return The frame, or nothing if Zstd refused.
		static std::optional<std::vector<std::byte>>
		Compress(std::span<const std::byte> payload, int level = DEFAULT_LEVEL);

		// Compresses against a dictionary. The common case.
		//
		// @param payload The group's concatenated, uncompressed bytes.
		// @param dictionary The dictionary to compress against.
		// @param level The compression level.
		// @return The frame, or nothing if Zstd refused.
		static std::optional<std::vector<std::byte>>
		Compress(std::span<const std::byte> payload, const Dictionary &dictionary, int level = DEFAULT_LEVEL);

		// Decompresses a frame into a buffer of exactly `expectedBytes`.
		//
		// **`expectedBytes` comes from the manifest, never from the frame.** A
		// frame header can claim any decompressed size, so sizing a buffer from
		// it is a decompression bomb: a few kilobytes on the wire become a
		// multi-gigabyte allocation. The manifest already records what a bundle
		// weighs and it is signed, so that is the number to believe.
		//
		// A frame that decompresses to anything other than exactly
		// `expectedBytes` is refused rather than truncated or padded.
		//
		// @param frame The compressed bytes.
		// @param expectedBytes The bundle's uncompressed length, from the
		//        manifest.
		// @return The payload, or nothing. Nothing means refuse the content.
		static std::optional<std::vector<std::byte>>
		Decompress(std::span<const std::byte> frame, uint64_t expectedBytes);

		// Decompresses a frame that was compressed against a dictionary.
		//
		// @param frame The compressed bytes.
		// @param dictionary The dictionary it was compressed against.
		// @param expectedBytes The bundle's uncompressed length, from the
		//        manifest.
		// @return The payload, or nothing.
		static std::optional<std::vector<std::byte>>
		Decompress(std::span<const std::byte> frame, const Dictionary &dictionary, uint64_t expectedBytes);

		// The largest payload this will decompress into, as a backstop.
		//
		// The manifest bound is the real check; this catches a manifest that is
		// itself absurd before the allocator does.
		static constexpr uint64_t MAXIMUM_PAYLOAD_BYTES = 4ull * 1024 * 1024 * 1024;
	};
}
