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
// small files sharing a great deal - the same vertex layouts, the same material
// fields, the same strings - and without one they compress as thousands of
// independently mediocre blobs. The dictionary is itself content-addressed and
// shipped as a group, so it versions like everything else instead of becoming an
// out-of-band file that can drift.
//
// **Decompression treats its input as hostile.** A frame arrives from an origin,
// and `repo_layout.md` §1 says anyone can run one. A frame header can claim any
// decompressed size it likes, so nothing here allocates against what the frame
// says - only against what the *manifest* already said the group weighs.
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
		// every group forever, so it is the cheapest large thing in the format -
		// but the *size* is still a number CDN.md §9 has no measurement behind.
		//
		// **What has been measured is that the dictionary works, and on which
		// groups.** `engine.delivery.bench.compression` reports it as worthless
		// on a 4 MiB group - 5.06x either way - and worth a fifth of the bytes
		// on small ones: two hundred 4 KiB groups compress 3.90x plain and 4.86x
		// against a dictionary.
		//
		// That is not a contradiction, it is what a dictionary is for. Zstd
		// builds its own history as it goes, so by a few kilobytes into a large
		// payload it has learned the content's vocabulary and a supplied one
		// adds nothing. A dictionary earns its keep only on payloads too short
		// to build that history - which is exactly what a group of small assets
		// is, and the case the format ships them in.
		//
		// So the row to watch when tuning this constant is the *small*-group
		// one. Measuring a dictionary against a multi-megabyte group will always
		// say it does nothing, and concluding that from it would be reading the
		// wrong benchmark rather than a fact about dictionaries.
		static constexpr size_t DEFAULT_TRAINED_BYTES = 110 * 1024;

		// Trains a dictionary over a sample of a game's content.
		//
		// Run once per published build, not per request - training is
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
		// legal and nearly useless - and a manifest shipped where a dictionary
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
		// **Measured, and the measurement overturned the previous value.** This
		// was 9, on the reasoning that "preparation happens once and streaming
		// happens many times, so the origin's CPU is the cheap side of that
		// trade". Both halves of that turned out to be wrong.
		//
		// `engine.delivery.bench.compression` over 4 MiB of scene-shaped content
		// reports, as ratio against nanoseconds per kibibyte:
		//
		//     level  1     4.82x        1463 ns/KiB
		//     level  3     5.00x        1647 ns/KiB   <- zstd's default
		//     level  9     5.06x        8837 ns/KiB   <- what this used to be
		//     level 15     5.53x      100272 ns/KiB
		//     level 19     5.80x      256786 ns/KiB
		//
		// **Level 9 was dominated in both directions.** Against 3 it cost 5.4
		// times the CPU for 1.2% fewer bytes; against 15 it gave up most of the
		// ratio that is actually available. It was the one setting on the curve
		// that bought neither thing.
		//
		// And the trade is not "once against many", because `cdn::Origin::Pump`
		// compresses **on a cache miss, with a client waiting**. For one 4 MiB
		// group that is the difference between a 6.7 ms first request and a 36 ms
		// one - and level 19 would make it 1.05 seconds, which is why the levels
		// where the ratio genuinely improves are unreachable from here rather
		// than merely expensive. Only once the prepared-group cache is warm does
		// the origin's CPU stop being a latency the player sees, and a default
		// cannot assume the warm case.
		//
		// So: 3, which is within 1.2% of what 9 achieved at a fifth of the miss
		// latency. Raise it per deployment through `Origin::CompressionLevel` if
		// that origin prepares ahead of demand rather than on it - that is the
		// arrangement the old reasoning described, and it is a deployment's to
		// choose rather than this constant's to assume.
		//
		// The corpus is synthetic - a repeating markup vocabulary with scattered
		// numeric noise. It stands in for scene and manifest text, and it does
		// **not** stand in for already-compressed textures or audio, where every
		// level converges on no gain. Re-measure against a real build before
		// treating the ratios above as anything but the shape of the curve.
		static constexpr int DEFAULT_LEVEL = 3;

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
