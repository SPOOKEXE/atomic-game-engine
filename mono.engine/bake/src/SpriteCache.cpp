#include <engine/assets/ContentHash.hpp>
#include <engine/bake/SpriteCache.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cryptopp/zlib.h>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>

namespace engine::bake {
	namespace {
		constexpr uint64_t CodecWorkspace = 128 * 1024;
		constexpr std::string_view Alphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		struct LimitFailure {};
		struct StreamFailure {};
		struct ShapeFailure {};
		struct Budget {
			uint64_t Available;
			bool Spend(uint64_t bytes) {
				if (bytes > Available) return false;
				Available -= bytes;
				return true;
			}
		};
		bool Fail(std::string &failure, std::string_view message) {
			failure = "sprite cache: ";
			failure += message;
			return false;
		}
		bool LayoutValid(SpriteCacheLayout layout) {
			return layout == SpriteCacheLayout::Rgba8TopDown || layout == SpriteCacheLayout::Bgra8TopDown ||
				   layout == SpriteCacheLayout::Rgba8BottomUp || layout == SpriteCacheLayout::Bgra8BottomUp;
		}
		std::optional<uint64_t> PixelBytes(uint64_t width, uint64_t height) {
			if (!width || !height || width > SpriteCacheLimits::MaximumDimension ||
				height > SpriteCacheLimits::MaximumDimension ||
				width * height > SpriteCacheLimits::MaximumPixels)
				return {};
			return width * height * 4;
		}
		template <class T> bool Reserve(std::vector<T> &values, size_t count, Budget &budget) {
			if (count > UINT64_MAX / sizeof(T) || !budget.Spend(count * sizeof(T))) return false;
			values.reserve(count);
			return budget.Spend((values.capacity() - count) * sizeof(T));
		}
		bool Reserve(std::string &text, size_t count, Budget &budget) {
			if (!budget.Spend(uint64_t(count) + 1)) return false;
			text.reserve(count);
			return budget.Spend(text.capacity() > count ? text.capacity() - count : 0);
		}
		class VectorSink final : public CryptoPP::Bufferless<CryptoPP::Sink> {
			std::vector<uint8_t> &Bytes;
			size_t Maximum;
			bool Ended = false;

		  public:
			VectorSink(std::vector<uint8_t> &bytes, size_t maximum) : Bytes(bytes), Maximum(maximum) {}
			void IsolatedInitialize(const CryptoPP::NameValuePairs &) override {}
			size_t Put2(const CryptoPP::byte *bytes, size_t length, int ended, bool) override {
				if (Ended && length) throw StreamFailure{};
				if (length > Maximum - Bytes.size()) throw LimitFailure{};
				if (length) Bytes.insert(Bytes.end(), bytes, bytes + length);
				Ended |= ended != 0;
				return 0;
			}
		};
		bool Decode64(std::string_view text, std::vector<uint8_t> &bytes) {
			if (text.empty() || text.size() % 4) return false;
			for (size_t at = 0; at < text.size(); at += 4) {
				std::array<unsigned, 4> values{};
				size_t padding = 0;
				for (size_t j = 0; j < 4; j++) {
					if (text[at + j] == '=') {
						if (j < 2 || at + 4 != text.size()) return false;
						padding++;
					} else {
						if (padding) return false;
						const auto n = Alphabet.find(text[at + j]);
						if (n == std::string_view::npos) return false;
						values[j] = unsigned(n);
					}
				}
				if (padding > 2 || (padding == 2 && (values[1] & 15)) || (padding == 1 && (values[2] & 3)))
					return false;
				const uint32_t word = values[0] << 18 | values[1] << 12 | values[2] << 6 | values[3];
				bytes.push_back(uint8_t(word >> 16));
				if (padding < 2) bytes.push_back(uint8_t(word >> 8));
				if (!padding) bytes.push_back(uint8_t(word));
			}
			return true;
		}
		void Encode64(std::span<const uint8_t> bytes, std::string &text) {
			for (size_t at = 0; at < bytes.size(); at += 3) {
				const size_t count = std::min(size_t{3}, bytes.size() - at);
				uint32_t word = uint32_t(bytes[at]) << 16;
				if (count > 1) word |= uint32_t(bytes[at + 1]) << 8;
				if (count > 2) word |= bytes[at + 2];
				text.push_back(Alphabet[(word >> 18) & 63]);
				text.push_back(Alphabet[(word >> 12) & 63]);
				text.push_back(count > 1 ? Alphabet[(word >> 6) & 63] : '=');
				text.push_back(count > 2 ? Alphabet[word & 63] : '=');
			}
		}
		void Convert(std::vector<uint8_t> &bytes, uint32_t width, uint32_t height, SpriteCacheLayout layout) {
			if (layout == SpriteCacheLayout::Bgra8TopDown || layout == SpriteCacheLayout::Bgra8BottomUp)
				for (size_t at = 0; at < bytes.size(); at += 4)
					std::swap(bytes[at], bytes[at + 2]);
			if (layout == SpriteCacheLayout::Rgba8BottomUp || layout == SpriteCacheLayout::Bgra8BottomUp) {
				const size_t stride = size_t(width) * 4;
				for (uint32_t row = 0; row < height / 2; row++)
					for (size_t column = 0; column < stride; column++)
						std::swap(
							bytes[size_t(row) * stride + column],
							bytes[size_t(height - 1 - row) * stride + column]
						);
			}
		}

		bool Number(const nlohmann::json &value, uint64_t &result) {
			if (!value.is_number()) return false;
			const double raw = value.get<double>();
			if (!std::isfinite(raw) || raw < 1 || raw > SpriteCacheLimits::MaximumDimension ||
				std::floor(raw) != raw)
				return false;
			result = uint64_t(raw);
			return true;
		}
	}
	std::string_view SpriteCacheLayoutName(SpriteCacheLayout layout) {
		switch (layout) {
		case SpriteCacheLayout::Rgba8TopDown:
			return "rgba8-top-down";
		case SpriteCacheLayout::Bgra8TopDown:
			return "bgra8-top-down";
		case SpriteCacheLayout::Rgba8BottomUp:
			return "rgba8-bottom-up";
		case SpriteCacheLayout::Bgra8BottomUp:
			return "bgra8-bottom-up";
		}
		return {};
	}
	std::optional<SpriteCacheLayout> ParseSpriteCacheLayoutName(std::string_view name) {
		for (const auto layout :
			 {SpriteCacheLayout::Rgba8TopDown,
			  SpriteCacheLayout::Bgra8TopDown,
			  SpriteCacheLayout::Rgba8BottomUp,
			  SpriteCacheLayout::Bgra8BottomUp})
			if (SpriteCacheLayoutName(layout) == name) return layout;
		return {};
	}
	std::optional<std::array<char, 64>> SpriteCacheDataHash(std::string_view text) {
		ENGINE_PROFILE_CAT("source sprite cache identity", engine::core::ProfileCategory::Engine);
		if (text.size() > SpriteCacheLimits::MaximumEncodedBytes) return {};
		const auto digest = engine::assets::Hasher::Of(std::as_bytes(std::span(text.data(), text.size())));
		constexpr std::string_view hex = "0123456789abcdef";
		std::array<char, 64> result{};
		for (size_t byte = 0; byte < digest.Digest.size(); byte++) {
			result[2 * byte] = hex[digest.Digest[byte] >> 4];
			result[2 * byte + 1] = hex[digest.Digest[byte] & 15];
		}
		engine::core::Metrics::Count("source sprite cache identity bytes", double(text.size()));
		return result;
	}
	bool ReadSpriteCache(
		std::string_view text,
		SpriteCacheLayout layout,
		std::vector<SpriteCacheFrame> &result,
		std::string &failure,
		uint64_t maximumBytes,
		SpriteCacheShape shape
	) {
		ENGINE_PROFILE_CAT("source sprite cache decode", engine::core::ProfileCategory::Engine);
		failure.clear();
		if (!LayoutValid(layout)) return Fail(failure, "an explicit supported byte layout is required");
		if (shape != SpriteCacheShape::Array && shape != SpriteCacheShape::Sprite)
			return Fail(failure, "cache shape is unknown");
		if (text.size() > SpriteCacheLimits::MaximumEncodedBytes)
			return Fail(failure, "encoded text exceeds its native limit");
		Budget budget{maximumBytes};
		if (!budget.Spend(CodecWorkspace) || !budget.Spend(result.capacity() * sizeof(SpriteCacheFrame)))
			return Fail(failure, "prior result and workspace exceed the byte limit");
		for (const auto &frame : result)
			if (!budget.Spend(frame.Rgba.capacity()))
				return Fail(failure, "prior pixels exceed the byte limit");
		try {
			const auto source =
				nlohmann::json::parse(text, [](int depth, nlohmann::json::parse_event_t, nlohmann::json &) {
					if (depth > 16) throw ShapeFailure{};
					return true;
				});
			if ((shape == SpriteCacheShape::Array &&
				 (!source.is_array() || source.size() > SpriteCacheLimits::MaximumFrames)) ||
				(shape == SpriteCacheShape::Sprite && !source.is_object()))
				return Fail(failure, "cache root does not match the source sprite shape");
			const size_t count = shape == SpriteCacheShape::Array ? source.size() : 1;
			std::vector<SpriteCacheFrame> candidate;
			if (!Reserve(candidate, count, budget))
				return Fail(failure, "frame table exceeds the byte limit");
			uint64_t totalPixels = 0;
			for (size_t index = 0; index < count; index++) {
				const auto &record = shape == SpriteCacheShape::Array ? source[index] : source;
				uint64_t width = 0, height = 0;
				if (!record.is_object() || !record.contains("width") || !record.contains("height") ||
					!record.contains("buffer") || !record["buffer"].is_string() ||
					!Number(record["width"], width) || !Number(record["height"], height))
					return Fail(failure, "frame shape has no supported source surface");
				const auto size = PixelBytes(width, height);
				if (!size || width * height > SpriteCacheLimits::MaximumPixels - totalPixels)
					return Fail(failure, "aggregate pixels exceed the native limit");
				totalPixels += width * height;
				const auto &encoded = record["buffer"].get_ref<const std::string &>();
				std::vector<uint8_t> compressed;
				if (!Reserve(compressed, encoded.size() / 4 * 3, budget))
					return Fail(failure, "compressed frame exceeds the byte limit");
				if (!Decode64(encoded, compressed)) return Fail(failure, "frame base64 is not canonical");
				SpriteCacheFrame frame;
				frame.Width = uint32_t(width);
				frame.Height = uint32_t(height);
				if (!Reserve(frame.Rgba, size_t(*size), budget))
					return Fail(failure, "decoded frame exceeds the byte limit");
				CryptoPP::ZlibDecompressor decoder(new VectorSink(frame.Rgba, size_t(*size)), false);
				decoder.Put(compressed.data(), compressed.size());
				decoder.MessageEnd();
				if (frame.Rgba.size() != *size)
					return Fail(failure, "decompressed bytes disagree with frame dimensions");
				engine::core::Metrics::Count("source sprite cache frames decoded", 1);
				engine::core::Metrics::Count(
					"source sprite cache pixels decoded bytes", double(frame.Rgba.size())
				);
				Convert(frame.Rgba, frame.Width, frame.Height, layout);
				candidate.push_back(std::move(frame));
			}
			result = std::move(candidate);
			return true;
		} catch (const ShapeFailure &) {
			return Fail(failure, "cache JSON nesting exceeds its source profile");
		} catch (const LimitFailure &) {
			return Fail(failure, "decompressed frame exceeds its exact surface size");
		} catch (const StreamFailure &) {
			return Fail(failure, "compressed frame contains trailing bytes");
		} catch (const CryptoPP::Exception &) {
			return Fail(failure, "compressed frame is invalid");
		} catch (const nlohmann::json::exception &) {
			return Fail(failure, "cache JSON is invalid");
		} catch (const std::bad_alloc &) {
			return Fail(failure, "owned allocation failed");
		}
	}
	bool WriteSpriteCache(
		std::span<const SpriteCacheFrame> frames,
		SpriteCacheLayout layout,
		std::string &result,
		std::string &failure,
		uint64_t maximumBytes,
		SpriteCacheShape shape
	) {
		ENGINE_PROFILE_CAT("source sprite cache encode", engine::core::ProfileCategory::Engine);
		failure.clear();
		if (!LayoutValid(layout)) return Fail(failure, "an explicit supported byte layout is required");
		if (shape != SpriteCacheShape::Array && shape != SpriteCacheShape::Sprite)
			return Fail(failure, "cache shape is unknown");
		if ((shape == SpriteCacheShape::Sprite && frames.size() != 1) ||
			frames.size() > SpriteCacheLimits::MaximumFrames)
			return Fail(failure, "frame count exceeds its native limit");
		Budget budget{maximumBytes};
		if (!budget.Spend(CodecWorkspace) || !budget.Spend(result.capacity() + 1))
			return Fail(failure, "prior text and workspace exceed the byte limit");
		uint64_t maximumText = 2, totalPixels = 0;
		for (const auto &frame : frames) {
			const auto bytes = PixelBytes(frame.Width, frame.Height);
			if (!bytes || *bytes != frame.Rgba.size() ||
				uint64_t(frame.Width) * frame.Height > SpriteCacheLimits::MaximumPixels - totalPixels)
				return Fail(failure, "frame bytes or aggregate pixels exceed their source shape");
			totalPixels += uint64_t(frame.Width) * frame.Height;
			const uint64_t compressed = *bytes + (*bytes / 16384 + 1) * 5 + 64;
			maximumText += 4 * ((compressed + 2) / 3) + 96;
		}
		maximumText = std::min(maximumText, SpriteCacheLimits::MaximumEncodedBytes);
		try {
			std::string candidate;
			if (!Reserve(candidate, size_t(maximumText), budget))
				return Fail(failure, "cache text exceeds the byte limit");
			if (shape == SpriteCacheShape::Array) candidate += '[';
			for (size_t index = 0; index < frames.size(); index++) {
				const auto &frame = frames[index];
				std::vector<uint8_t> raw, compressed;
				const size_t worst = frame.Rgba.size() + (frame.Rgba.size() / 16384 + 1) * 5 + 64;
				if (!Reserve(raw, frame.Rgba.size(), budget) || !Reserve(compressed, worst, budget))
					return Fail(failure, "compression workspace exceeds the byte limit");
				raw.assign(frame.Rgba.begin(), frame.Rgba.end());
				Convert(raw, frame.Width, frame.Height, layout);
				CryptoPP::ZlibCompressor encoder(new VectorSink(compressed, worst));
				encoder.Put(raw.data(), raw.size());
				encoder.MessageEnd();
				engine::core::Metrics::Count("source sprite cache frames encoded", 1);
				engine::core::Metrics::Count(
					"source sprite cache compressed bytes", double(compressed.size())
				);
				if (4 * ((compressed.size() + 2) / 3) + 96 > maximumText - candidate.size())
					return Fail(failure, "cache text exceeds its native limit");
				if (index) candidate += ',';
				candidate += "{\"width\":";
				candidate += std::to_string(frame.Width);
				candidate += ",\"height\":";
				candidate += std::to_string(frame.Height);
				candidate += ",\"buffer\":\"";
				Encode64(compressed, candidate);
				candidate += "\"}";
			}
			if (shape == SpriteCacheShape::Array) candidate += ']';
			result = std::move(candidate);
			return true;
		} catch (const LimitFailure &) {
			return Fail(failure, "compressed frame exceeds its preadmitted size");
		} catch (const StreamFailure &) {
			return Fail(failure, "compressed frame contains trailing bytes");
		} catch (const CryptoPP::Exception &) {
			return Fail(failure, "frame compression failed");
		} catch (const std::bad_alloc &) {
			return Fail(failure, "owned allocation failed");
		}
	}
}
