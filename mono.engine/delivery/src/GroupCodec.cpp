#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/delivery/GroupCodec.hpp>

#include <cstring>
#include <zdict.h>
#include <zstd.h>

namespace engine::delivery {

	namespace {
		using engine::assets::ContentHash;
		using engine::assets::Hasher;

		// A Zstd context, closed however the function leaves.
		//
		// The C API hands back raw pointers with paired free functions, and
		// every early return below is a leak without this. There are six such
		// returns, which is exactly the count at which remembering stops working.
		template <typename Context, void (*Free)(Context *)> class Owned {
		  public:
			explicit Owned(Context *context) : Held(context) {}
			~Owned() {
				if (Held != nullptr) {
					Free(Held);
				}
			}

			Owned(const Owned &) = delete;
			Owned &operator=(const Owned &) = delete;

			Context *Get() const {
				return Held;
			}
			explicit operator bool() const {
				return Held != nullptr;
			}

		  private:
			Context *Held;
		};

		void FreeCompress(ZSTD_CCtx *context) {
			ZSTD_freeCCtx(context);
		}
		void FreeDecompress(ZSTD_DCtx *context) {
			ZSTD_freeDCtx(context);
		}

		using CompressContext = Owned<ZSTD_CCtx, FreeCompress>;
		using DecompressContext = Owned<ZSTD_DCtx, FreeDecompress>;

		std::optional<std::vector<std::byte>> CompressWith(
			std::span<const std::byte> payload, const std::byte *dictionary, size_t dictionaryBytes, int level
		) {
			CompressContext context(ZSTD_createCCtx());
			if (!context) {
				return std::nullopt;
			}

			// The advanced API rather than ZSTD_compressCCtx, for one parameter:
			// the content checksum.
			//
			// **Zstd leaves it off by default**, and with it off a frame with a
			// flipped byte decompresses cleanly to the right length and the
			// wrong content. The chunk hashes would still catch that downstream
			// — a client verifies everything against the signed manifest root —
			// but only after a whole group has been transferred and expanded,
			// and with nothing saying the transport was at fault rather than the
			// origin. Four bytes per frame buys the error where it happened.
			if (ZSTD_isError(ZSTD_CCtx_setParameter(context.Get(), ZSTD_c_compressionLevel, level)) ||
				ZSTD_isError(ZSTD_CCtx_setParameter(context.Get(), ZSTD_c_checksumFlag, 1))) {
				return std::nullopt;
			}

			if (dictionary != nullptr &&
				ZSTD_isError(ZSTD_CCtx_loadDictionary(context.Get(), dictionary, dictionaryBytes))) {
				return std::nullopt;
			}

			const size_t bound = ZSTD_compressBound(payload.size());
			std::vector<std::byte> frame(bound);

			const size_t written =
				ZSTD_compress2(context.Get(), frame.data(), frame.size(), payload.data(), payload.size());

			if (ZSTD_isError(written)) {
				engine::core::Metrics::Count("cdn.codec.compress.failed", 1.0);
				return std::nullopt;
			}

			frame.resize(written);
			engine::core::Metrics::Count("cdn.codec.compressed.in", static_cast<double>(payload.size()));
			engine::core::Metrics::Count("cdn.codec.compressed.out", static_cast<double>(written));
			return frame;
		}

		std::optional<std::vector<std::byte>> DecompressWith(
			std::span<const std::byte> frame,
			const std::byte *dictionary,
			size_t dictionaryBytes,
			uint64_t expectedBytes
		) {
			const auto refuse = []() -> std::optional<std::vector<std::byte>> {
				engine::core::Metrics::Count("cdn.codec.decompress.refused", 1.0);
				return std::nullopt;
			};

			if (frame.empty() || expectedBytes == 0 || expectedBytes > GroupCodec::MAXIMUM_PAYLOAD_BYTES) {
				return refuse();
			}

			// The buffer is sized from `expectedBytes` and nothing else.
			//
			// ZSTD_getFrameContentSize would answer what the *frame* claims, and
			// a frame is attacker-controlled: a few kilobytes on the wire can
			// declare a multi-gigabyte payload, and sizing against that is the
			// classic decompression bomb. The manifest already records what this
			// bundle weighs and the manifest is signed, so that is the number
			// worth believing.
			std::vector<std::byte> payload(static_cast<size_t>(expectedBytes));

			DecompressContext context(ZSTD_createDCtx());
			if (!context) {
				return refuse();
			}

			size_t written = 0;
			if (dictionary != nullptr) {
				written = ZSTD_decompress_usingDict(
					context.Get(),
					payload.data(),
					payload.size(),
					frame.data(),
					frame.size(),
					dictionary,
					dictionaryBytes
				);
			} else {
				written = ZSTD_decompressDCtx(
					context.Get(), payload.data(), payload.size(), frame.data(), frame.size()
				);
			}

			if (ZSTD_isError(written)) {
				return refuse();
			}

			// Exactly, not at most. A frame that decompresses short is content
			// that does not match what the manifest describes, and truncating or
			// padding to fit would hand the hash check something the origin
			// never sent.
			if (written != expectedBytes) {
				return refuse();
			}

			engine::core::Metrics::Count("cdn.codec.decompressed", 1.0);
			return payload;
		}
	}

	std::optional<Dictionary>
	Dictionary::Train(std::span<const std::span<const std::byte>> samples, size_t capacityBytes) {
		ENGINE_PROFILE_CAT("Dictionary::Train", core::ProfileCategory::Assets);

		if (samples.empty() || capacityBytes == 0) {
			return std::nullopt;
		}

		// ZDICT wants the samples end to end in one buffer plus their lengths,
		// so they are flattened here rather than the caller being asked to
		// present them that way — a caller holding a vector of files should not
		// have to know the trainer's calling convention.
		std::vector<std::byte> flattened;
		std::vector<size_t> sizes;
		sizes.reserve(samples.size());
		for (const std::span<const std::byte> &sample : samples) {
			if (sample.empty()) {
				continue;
			}
			flattened.insert(flattened.end(), sample.begin(), sample.end());
			sizes.push_back(sample.size());
		}

		if (sizes.empty()) {
			return std::nullopt;
		}

		std::vector<std::byte> trained(capacityBytes);
		const size_t written = ZDICT_trainFromBuffer(
			trained.data(),
			trained.size(),
			flattened.data(),
			sizes.data(),
			static_cast<unsigned>(sizes.size())
		);

		if (ZDICT_isError(written)) {
			// Passed through rather than papered over. A dictionary trained on
			// too little is worse than none: it ships to every client and costs
			// bytes on every fetch while buying nothing.
			engine::core::Metrics::Count("cdn.codec.train.failed", 1.0);
			return std::nullopt;
		}

		trained.resize(written);
		return Load(trained);
	}

	std::optional<Dictionary> Dictionary::Load(std::span<const std::byte> bytes) {
		if (bytes.empty()) {
			return std::nullopt;
		}

		// Zstd will happily use arbitrary bytes as a "raw content" dictionary,
		// which is legal and useless. Refusing anything without the trained
		// dictionary's magic keeps a misconfiguration — a manifest shipped where
		// a dictionary was expected — from silently costing ratio on every group
		// for the life of a deployment.
		if (ZDICT_getDictID(bytes.data(), bytes.size()) == 0) {
			return std::nullopt;
		}

		Dictionary dictionary;
		dictionary.Raw.assign(bytes.begin(), bytes.end());
		dictionary.Address = Hasher::Of(bytes);
		return dictionary;
	}

	std::optional<std::vector<std::byte>>
	GroupCodec::Compress(std::span<const std::byte> payload, int level) {
		ENGINE_PROFILE_CAT("GroupCodec::Compress", core::ProfileCategory::Assets);
		return CompressWith(payload, nullptr, 0, level);
	}

	std::optional<std::vector<std::byte>>
	GroupCodec::Compress(std::span<const std::byte> payload, const Dictionary &dictionary, int level) {
		ENGINE_PROFILE_CAT("GroupCodec::Compress", core::ProfileCategory::Assets);
		return CompressWith(payload, dictionary.Bytes().data(), dictionary.Bytes().size(), level);
	}

	std::optional<std::vector<std::byte>>
	GroupCodec::Decompress(std::span<const std::byte> frame, uint64_t expectedBytes) {
		ENGINE_PROFILE_CAT("GroupCodec::Decompress", core::ProfileCategory::Assets);
		return DecompressWith(frame, nullptr, 0, expectedBytes);
	}

	std::optional<std::vector<std::byte>> GroupCodec::Decompress(
		std::span<const std::byte> frame, const Dictionary &dictionary, uint64_t expectedBytes
	) {
		ENGINE_PROFILE_CAT("GroupCodec::Decompress", core::ProfileCategory::Assets);
		return DecompressWith(frame, dictionary.Bytes().data(), dictionary.Bytes().size(), expectedBytes);
	}
}
