#include <engine/audio/Wav.hpp>
#include <engine/core/Metrics.hpp>

#include <cmath>
#include <cstring>
#include <vector>

// RIFF/WAV, parsed defensively.
//
// The format is a chain of length-prefixed chunks - a list of numbers telling a
// parser how far to jump - so the whole of the safety here is that **no length
// is acted on before it is checked against what actually arrived**. Every
// refusal below returns nothing rather than a shorter sound, because a shorter
// sound plays and a refusal does not.

namespace engine::audio {
	namespace {
		constexpr size_t RIFF_HEADER_BYTES = 12;
		constexpr size_t CHUNK_HEADER_BYTES = 8;
		constexpr size_t MINIMUM_FMT_BYTES = 16;

		// The two `wFormatTag` values this decodes. Everything else has a codec
		// behind it.
		constexpr uint16_t FORMAT_PCM = 0x0001;
		constexpr uint16_t FORMAT_FLOAT = 0x0003;

		// A `fmt ` chunk may declare this and put the real tag in its extension,
		// which is what any tool writing more than two channels emits.
		constexpr uint16_t FORMAT_EXTENSIBLE = 0xFFFE;

		bool Tag(std::span<const std::byte> bytes, size_t offset, const char (&name)[5]) {
			if (offset + 4 > bytes.size()) {
				return false;
			}
			return std::memcmp(bytes.data() + offset, name, 4) == 0;
		}

		// Little-endian, read a byte at a time rather than memcpy'd into a
		// wider type: the file's byte order is the format's and must not depend
		// on the host's.
		uint16_t ReadU16(std::span<const std::byte> bytes, size_t offset) {
			return static_cast<uint16_t>(
				static_cast<uint16_t>(bytes[offset]) | (static_cast<uint16_t>(bytes[offset + 1]) << 8)
			);
		}

		uint32_t ReadU32(std::span<const std::byte> bytes, size_t offset) {
			return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
				   (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
				   (static_cast<uint32_t>(bytes[offset + 3]) << 24);
		}

		// What `fmt ` said.
		struct WaveFormat {
			uint16_t Encoding = 0;
			uint16_t Channels = 0;
			uint32_t SampleRate = 0;
			uint16_t BitsPerSample = 0;
		};

		float FromSigned16(int16_t value) {
			// Divided by 32768 rather than 32767, so the scale is exact and
			// -1.0 is representable. The alternative makes every sample a
			// fraction too loud and full-scale negative clip.
			return static_cast<float>(value) / 32768.0f;
		}

		std::optional<SampleBuffer>
		Convert(std::span<const std::byte> data, const WaveFormat &format, const AudioFormat &shape) {
			const size_t bytesPerSample = format.BitsPerSample / 8u;
			const size_t sampleCount = data.size() / bytesPerSample;
			// Whole frames only. A trailing partial frame is a truncated file,
			// and rounding it away would be the clamp this file refuses.
			if (sampleCount % format.Channels != 0) {
				return std::nullopt;
			}

			std::vector<float> samples(sampleCount);
			for (size_t index = 0; index < sampleCount; ++index) {
				const size_t at = index * bytesPerSample;
				switch (format.BitsPerSample) {
				case 8:
					// Unsigned, centred on 128 - the one encoding in this
					// format that is not signed, and forgetting it is a
					// full-scale DC offset rather than a subtle error.
					samples[index] = (static_cast<float>(static_cast<uint8_t>(data[at])) - 128.0f) / 128.0f;
					break;
				case 16:
					samples[index] = FromSigned16(static_cast<int16_t>(ReadU16(data, at)));
					break;
				case 24: {
					// Sign-extended from 24 bits by hand: there is no
					// 24-bit integer to let the compiler do it.
					const uint32_t raw = static_cast<uint32_t>(data[at]) |
										 (static_cast<uint32_t>(data[at + 1]) << 8) |
										 (static_cast<uint32_t>(data[at + 2]) << 16);
					const int32_t signedValue = (raw & 0x800000u) != 0
													? static_cast<int32_t>(raw | 0xFF000000u)
													: static_cast<int32_t>(raw);
					samples[index] = static_cast<float>(signedValue) / 8388608.0f;
					break;
				}
				case 32: {
					float value = 0.0f;
					const uint32_t raw = ReadU32(data, at);
					// Through memcpy rather than a reinterpret_cast: the
					// latter is undefined and the former compiles to
					// nothing.
					std::memcpy(&value, &raw, sizeof(value));
					if (!std::isfinite(value)) {
						return std::nullopt;
					}
					samples[index] = value;
					break;
				}
				default:
					return std::nullopt;
				}
			}
			return SampleBuffer(shape, samples);
		}
		struct ParsedWave {
			WaveFormat Format;
			std::span<const std::byte> Data;
			uint32_t FormatBytes = 0;
			uint16_t OriginalEncoding = 0;
			uint16_t BlockAlign = 0;
		};

		std::optional<ParsedWave> ParseWave(std::span<const std::byte> bytes) {
			if (bytes.size() > MAXIMUM_WAV_BYTES || !IsWav(bytes)) return std::nullopt;
			ParsedWave parsed;
			WaveFormat &format = parsed.Format;
			bool haveFormat = false;
			std::span<const std::byte> data;
			bool haveData = false;

			size_t at = RIFF_HEADER_BYTES;
			while (at + CHUNK_HEADER_BYTES <= bytes.size()) {
				const uint32_t declared = ReadU32(bytes, at + 4);
				const size_t body = at + CHUNK_HEADER_BYTES;

				// The check that matters. A chunk claiming to run past the end of
				// what arrived is a refusal, not a clamp: clamping turns a
				// truncated file into a shorter sound that plays, and the
				// corruption is then inaudible until somebody wonders why a
				// footstep got quieter.
				if (declared > bytes.size() - body) {
					return std::nullopt;
				}

				if (Tag(bytes, at, "fmt ")) {
					if (declared < MINIMUM_FMT_BYTES) {
						return std::nullopt;
					}
					parsed.FormatBytes = declared;
					parsed.OriginalEncoding = ReadU16(bytes, body);
					parsed.BlockAlign = ReadU16(bytes, body + 12);
					format.Encoding = parsed.OriginalEncoding;
					format.Channels = ReadU16(bytes, body + 2);
					format.SampleRate = ReadU32(bytes, body + 4);
					format.BitsPerSample = ReadU16(bytes, body + 14);

					if (format.Encoding == FORMAT_EXTENSIBLE) {
						// The real tag is the first two bytes of the extension's
						// GUID. A file that says "extensible" and gives no
						// extension is malformed rather than assumed to be PCM.
						if (declared < 26) {
							return std::nullopt;
						}
						format.Encoding = ReadU16(bytes, body + 24);
					}
					haveFormat = true;
				} else if (Tag(bytes, at, "data")) {
					data = bytes.subspan(body, declared);
					haveData = true;
				}

				// Chunks are word-aligned: an odd length is followed by a pad byte
				// that is not counted in the length. Missing this reads every
				// subsequent chunk header one byte off, which looks like a corrupt
				// file rather than a parser bug.
				const size_t advance = declared + (declared % 2);
				if (advance > bytes.size() - body) {
					// The pad byte would be past the end. The file ends here
					// legitimately, so stop rather than refuse - what has been
					// collected is checked below.
					break;
				}
				at = body + advance;
			}

			if (!haveFormat || !haveData) {
				return std::nullopt;
			}
			const AudioFormat shape{.SampleRate = format.SampleRate, .Channels = format.Channels};
			if (!shape.IsValid() || format.BitsPerSample == 0 || format.BitsPerSample % 8 != 0)
				return std::nullopt;
			const size_t frameBytes = (format.BitsPerSample / 8u) * format.Channels;
			if (data.size() % frameBytes != 0) return std::nullopt;
			parsed.Data = data;
			return parsed;
		}

	}

	bool IsWav(std::span<const std::byte> bytes) {
		if (bytes.size() < RIFF_HEADER_BYTES) {
			return false;
		}
		return Tag(bytes, 0, "RIFF") && Tag(bytes, 8, "WAVE");
	}

	std::optional<WavMetadata> InspectWav(std::span<const std::byte> bytes) {
		const auto parsed = ParseWave(bytes);
		if (!parsed) return std::nullopt;
		return WavMetadata{
			{parsed->Format.SampleRate, parsed->Format.Channels},
			parsed->Data.size() / (parsed->Format.BitsPerSample / 8)
		};
	}

	std::optional<SampleBuffer> DecodeWav(std::span<const std::byte> bytes) {
		return DecodeWav(bytes, MAXIMUM_WAV_BYTES);
	}

	std::optional<SampleBuffer> DecodeWav(std::span<const std::byte> bytes, size_t maximumSamples) {
		const auto refuse = []() -> std::optional<SampleBuffer> {
			core::Metrics::Count("audio.wav.refused", 1.0);
			return std::nullopt;
		};

		const auto parsed = ParseWave(bytes);
		if (!parsed) return refuse();
		const WaveFormat &format = parsed->Format;
		const auto data = parsed->Data;
		if (format.Encoding != FORMAT_PCM && format.Encoding != FORMAT_FLOAT) {
			// A-law, µ-law, ADPCM and everything else with a codec behind it.
			// Refused rather than guessed at: a decoder that guessed would
			// produce noise at full volume, which is the worst failure this
			// subsystem has.
			return refuse();
		}
		// The mixer decode contract supports float32, but not integer PCM32.
		// Pixel Composer PCM32 uses the separate double-preserving decode path.
		if (format.Encoding == FORMAT_FLOAT && format.BitsPerSample != 32) {
			return refuse();
		}
		if (format.Encoding == FORMAT_PCM && format.BitsPerSample == 32) {
			return refuse();
		}
		if (format.BitsPerSample != 8 && format.BitsPerSample != 16 && format.BitsPerSample != 24 &&
			format.BitsPerSample != 32) {
			return refuse();
		}

		if (data.size() / (format.BitsPerSample / 8) > maximumSamples) return refuse();
		const AudioFormat shape{.SampleRate = format.SampleRate, .Channels = format.Channels};
		std::optional<SampleBuffer> decoded = Convert(data, format, shape);
		if (!decoded) {
			return refuse();
		}
		core::Metrics::Count("audio.wav.decoded", 1.0);
		return decoded;
	}

	std::optional<PixelComposerWav>
	DecodePixelComposerWav(std::span<const std::byte> bytes, size_t maximumSamples) {
		const auto parsed = ParseWave(bytes);
		if (!parsed || parsed->FormatBytes != 16 || parsed->OriginalEncoding != FORMAT_PCM)
			return std::nullopt;
		const WaveFormat &format = parsed->Format;
		if (format.BitsPerSample != 8 && format.BitsPerSample != 16 && format.BitsPerSample != 32)
			return std::nullopt;
		const size_t stride = format.BitsPerSample / 8;
		if (parsed->BlockAlign != stride * format.Channels) return std::nullopt;
		const size_t count = parsed->Data.size() / stride;
		if (count > maximumSamples) return std::nullopt;
		PixelComposerWav decoded{{format.SampleRate, format.Channels}, {}};
		decoded.Samples.resize(count);
		for (size_t index = 0; index < count; index++) {
			const size_t offset = index * stride;
			if (stride == 1)
				decoded.Samples[index] = static_cast<uint8_t>(parsed->Data[offset]) / 128.0;
			else if (stride == 2)
				decoded.Samples[index] = static_cast<int16_t>(ReadU16(parsed->Data, offset)) / 32768.0;
			else
				decoded.Samples[index] = static_cast<int32_t>(ReadU32(parsed->Data, offset)) / 2147483648.0;
		}
		return decoded;
	}

}
