#include "Decoders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Baseline JPEG.
//
// **Baseline only, and progressive is refused by name.** A progressive file is
// a different decoder — the coefficients arrive across several scans and are
// refined rather than being complete after one — and half-reading one produces
// a recognisable, blurred, wrong picture. That is the failure mode this file
// refuses to have, for `Png.cpp`'s reason about interlacing.
//
// The IDCT here is the separable float one out of the definition rather than a
// fast integer approximation. This is a publishing step that runs once per
// texture on somebody's build machine; buying a few milliseconds by introducing
// a rounding difference from every other decoder in the world would be a bad
// trade, and "our bake output differs from the artist's preview by one level"
// is an expensive bug to chase.

namespace engine::bake {

	namespace {
		// The zig-zag order a quantisation table and a coefficient block are
		// stored in. Written out rather than generated, because the generator
		// is harder to check by eye than the table is.
		constexpr std::array<uint8_t, 64> ZIGZAG{
			0,	1,	8,	16, 9,	2,	3,	10, 17, 24, 32, 25, 18, 11, 4,	5,	12, 19, 26, 33, 40, 48,
			41, 34, 27, 20, 13, 6,	7,	14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
			30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
		};

		// A Huffman table, as the decoder wants it rather than as the file
		// stores it: the file gives code lengths and values, and this is the
		// canonical code derived from them.
		struct HuffmanTable {
			// The smallest and largest code of each length, and where that
			// length's values start in `Values`. Index one to sixteen.
			std::array<int32_t, 17> Minimum{};
			std::array<int32_t, 17> Maximum{};
			std::array<int32_t, 17> Offset{};
			std::vector<uint8_t> Values;
			bool Present = false;
		};

		struct Component {
			uint8_t Identifier = 0;
			uint8_t HorizontalSampling = 1;
			uint8_t VerticalSampling = 1;
			uint8_t QuantisationTable = 0;
			uint8_t DcTable = 0;
			uint8_t AcTable = 0;

			// The component's own plane, at its own resolution, before
			// upsampling to the image's.
			std::vector<uint8_t> Plane;
			uint32_t PlaneWidth = 0;
			uint32_t PlaneHeight = 0;
			int32_t Predictor = 0;
		};

		// Reads the entropy-coded segment a bit at a time, undoing the byte
		// stuffing as it goes.
		class BitReader {
		  public:
			BitReader(std::span<const std::byte> bytes, size_t offset) : Bytes(bytes), Offset(offset) {}

			// Reads one bit, MSB first.
			//
			// **A 0xFF byte in the entropy stream is followed by a stuffed
			// 0x00**, which is how a marker stays findable inside compressed
			// data. Consuming the stuffed byte here rather than pre-scanning
			// the segment is what keeps `Offset` pointing at the real marker
			// when the scan ends.
			int Bit() {
				if (Count == 0) {
					if (Offset >= Bytes.size()) {
						Exhausted = true;
						return 0;
					}
					Current = static_cast<uint8_t>(Bytes[Offset++]);
					if (Current == 0xFF) {
						if (Offset < Bytes.size() && static_cast<uint8_t>(Bytes[Offset]) == 0x00) {
							Offset++;
						} else {
							// A real marker. Back up so the caller finds it and
							// feed zeroes, which is what a truncated scan
							// decodes as rather than reading into the marker.
							Offset--;
							Exhausted = true;
							return 0;
						}
					}
					Count = 8;
				}
				Count--;
				return (Current >> Count) & 1;
			}

			int32_t Bits(int length) {
				int32_t value = 0;
				for (int index = 0; index < length; index++) {
					value = (value << 1) | Bit();
				}
				return value;
			}

			// Drops the partial byte, which is what a restart marker requires.
			void Align() {
				Count = 0;
			}

			bool Done() const {
				return Exhausted;
			}

			size_t Position() const {
				return Offset;
			}

			void Seek(size_t offset) {
				Offset = offset;
				Count = 0;
				Exhausted = false;
			}

		  private:
			std::span<const std::byte> Bytes;
			size_t Offset = 0;
			uint8_t Current = 0;
			int Count = 0;
			bool Exhausted = false;
		};

		// Turns a Huffman-coded magnitude and its bits into a signed value.
		//
		// The negative half of each magnitude band is stored as the low end of
		// the band, so a value whose top bit is clear is negative and is
		// recovered by adding the band's span. Getting this backwards produces
		// an image with the right structure and inverted contrast.
		int32_t Extend(int32_t value, int length) {
			if (length == 0) {
				return 0;
			}
			return value < (1 << (length - 1)) ? value - (1 << length) + 1 : value;
		}

		int32_t DecodeHuffman(BitReader &reader, const HuffmanTable &table, bool &failed) {
			int32_t code = 0;
			for (int length = 1; length <= 16; length++) {
				code = (code << 1) | reader.Bit();
				if (table.Maximum[length] >= 0 && code <= table.Maximum[length] &&
					code >= table.Minimum[length]) {
					const size_t index = static_cast<size_t>(table.Offset[length]) +
										 static_cast<size_t>(code - table.Minimum[length]);
					if (index >= table.Values.size()) {
						failed = true;
						return 0;
					}
					return table.Values[index];
				}
			}
			failed = true;
			return 0;
		}

		// The inverse DCT, separable and done in floats.
		void InverseTransform(const std::array<int32_t, 64> &coefficients, std::array<uint8_t, 64> &out) {
			// cos((2x+1) * u * pi / 16) * (u == 0 ? 1/sqrt2 : 1), precomputed on
			// the first call. A function-local static rather than a namespace
			// one so it is built after whatever it depends on and only when a
			// JPEG is actually decoded.
			static const std::array<float, 64> BASIS = [] {
				std::array<float, 64> table{};
				for (int x = 0; x < 8; x++) {
					for (int u = 0; u < 8; u++) {
						const float scale = u == 0 ? 0.353553390f : 0.5f;
						table[x * 8 + u] =
							scale * std::cos(static_cast<float>((2 * x + 1) * u) * 3.14159265f / 16.0f);
					}
				}
				return table;
			}();

			std::array<float, 64> rows{};
			for (int y = 0; y < 8; y++) {
				for (int x = 0; x < 8; x++) {
					float total = 0.0f;
					for (int u = 0; u < 8; u++) {
						total += BASIS[x * 8 + u] * static_cast<float>(coefficients[y * 8 + u]);
					}
					rows[y * 8 + x] = total;
				}
			}

			for (int x = 0; x < 8; x++) {
				for (int y = 0; y < 8; y++) {
					float total = 0.0f;
					for (int v = 0; v < 8; v++) {
						total += BASIS[y * 8 + v] * rows[v * 8 + x];
					}

					// Level shift by 128 and clamp: the transform's output is
					// centred on zero and a sharp edge legitimately overshoots
					// past both ends of the byte range.
					const int value = static_cast<int>(std::lround(total)) + 128;
					out[y * 8 + x] = static_cast<uint8_t>(std::clamp(value, 0, 255));
				}
			}
		}

		// Brings one component's plane up to the image's resolution.
		//
		// **Triangle-filtered on any axis that is subsampled by exactly two,
		// and nearest everywhere else — which is what libjpeg does, and
		// matching libjpeg is the whole requirement.** The first version of
		// this sampled nearest on every axis with a comment claiming that was
		// what every other decoder produces. It is not: measured against
		// Pillow, nearest chroma was 28 levels out on a saturated edge and this
		// is 3, which is the IDCT's own rounding. A bake step whose output
		// visibly disagrees with the artist's preview is an expensive bug to
		// chase, and it would have been ours.
		//
		// The weights are libjpeg's `h2v1_fancy_upsample` and
		// `h2v2_fancy_upsample` verbatim, rounding included: a nearer sample
		// counts three times and its neighbour once, and the two outputs of one
		// input round in opposite directions so a flat field stays flat.
		std::vector<uint8_t> Upsample(
			const Component &component,
			uint32_t width,
			uint32_t height,
			uint32_t maximumHorizontal,
			uint32_t maximumVertical
		) {
			std::vector<uint8_t> full(static_cast<size_t>(width) * height);

			const bool doubleHorizontal = component.HorizontalSampling * 2 == maximumHorizontal;
			const bool doubleVertical = component.VerticalSampling * 2 == maximumVertical;

			const auto at = [&component](uint32_t x, uint32_t y) {
				return static_cast<int32_t>(
					component.Plane
						[static_cast<size_t>(std::min(y, component.PlaneHeight - 1)) * component.PlaneWidth +
						 std::min(x, component.PlaneWidth - 1)]
				);
			};

			// The horizontal extent of the component's own plane that actually
			// holds image, rather than the block padding beyond it.
			const uint32_t sourceWidth = std::max<uint32_t>(
				1, (width * component.HorizontalSampling + maximumHorizontal - 1) / maximumHorizontal
			);
			const uint32_t sourceHeight = std::max<uint32_t>(
				1, (height * component.VerticalSampling + maximumVertical - 1) / maximumVertical
			);

			std::vector<int32_t> columnSums(width);

			for (uint32_t y = 0; y < height; y++) {
				// Vertically first, into a running sum scaled by four, so the
				// horizontal pass can do both filters in one rounding step.
				if (doubleVertical) {
					const uint32_t near = std::min(y / 2, sourceHeight - 1);
					const uint32_t far =
						(y % 2 == 0) ? (near == 0 ? near : near - 1) : std::min(near + 1, sourceHeight - 1);
					for (uint32_t x = 0; x < sourceWidth; x++) {
						columnSums[x] = 3 * at(x, near) + at(x, far);
					}
				} else {
					const uint32_t row =
						std::min(y * component.VerticalSampling / maximumVertical, sourceHeight - 1);
					for (uint32_t x = 0; x < sourceWidth; x++) {
						columnSums[x] = 4 * at(x, row);
					}
				}

				uint8_t *destination = full.data() + static_cast<size_t>(y) * width;
				for (uint32_t x = 0; x < width; x++) {
					if (doubleHorizontal) {
						const uint32_t near = std::min(x / 2, sourceWidth - 1);
						const uint32_t far = (x % 2 == 0) ? (near == 0 ? near : near - 1)
														  : std::min(near + 1, sourceWidth - 1);
						const int32_t rounding = (x % 2 == 0) ? 8 : 7;
						destination[x] = static_cast<uint8_t>(
							std::clamp((3 * columnSums[near] + columnSums[far] + rounding) >> 4, 0, 255)
						);
					} else {
						const uint32_t column =
							std::min(x * component.HorizontalSampling / maximumHorizontal, sourceWidth - 1);
						destination[x] =
							static_cast<uint8_t>(std::clamp((columnSums[column] + 2) >> 2, 0, 255));
					}
				}
			}
			return full;
		}
	}

	bool ReadJpeg(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure) {
		if (bytes.size() < 4 || static_cast<uint8_t>(bytes[0]) != 0xFF ||
			static_cast<uint8_t>(bytes[1]) != 0xD8) {
			failure = "jpeg: wrong signature";
			return false;
		}

		std::array<std::array<uint16_t, 64>, 4> quantisation{};
		std::array<HuffmanTable, 4> dcTables{};
		std::array<HuffmanTable, 4> acTables{};
		std::vector<Component> components;

		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t restartInterval = 0;
		bool haveFrame = false;

		size_t offset = 2;
		while (offset + 1 < bytes.size()) {
			if (static_cast<uint8_t>(bytes[offset]) != 0xFF) {
				offset++;
				continue;
			}

			const uint8_t marker = static_cast<uint8_t>(bytes[offset + 1]);
			offset += 2;

			// Standalone markers carry no length.
			if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
				continue;
			}
			if (marker == 0xD9) {
				break;
			}
			if (offset + 2 > bytes.size()) {
				failure = "jpeg: segment header past the end of the file";
				return false;
			}

			const size_t length =
				(static_cast<size_t>(bytes[offset]) << 8) | static_cast<size_t>(bytes[offset + 1]);
			if (length < 2 || offset + length > bytes.size()) {
				failure = "jpeg: segment runs past the end of the file";
				return false;
			}

			const std::span<const std::byte> segment = bytes.subspan(offset + 2, length - 2);

			switch (marker) {
			case 0xC0:
			case 0xC1: {
				// SOF0 baseline and SOF1 extended sequential. The two differ
				// only in a precision this decoder already checks.
				if (segment.size() < 6) {
					failure = "jpeg: malformed frame header";
					return false;
				}
				if (static_cast<uint8_t>(segment[0]) != 8) {
					failure = "jpeg: only 8-bit samples are supported";
					return false;
				}
				height = (static_cast<uint32_t>(segment[1]) << 8) | static_cast<uint32_t>(segment[2]);
				width = (static_cast<uint32_t>(segment[3]) << 8) | static_cast<uint32_t>(segment[4]);

				const size_t count = static_cast<uint8_t>(segment[5]);
				if (count != 1 && count != 3) {
					// Four components is CMYK or YCCK, which needs an Adobe
					// transform flag and an inversion nobody agrees about.
					failure = "jpeg: only greyscale and YCbCr images are supported";
					return false;
				}
				if (segment.size() < 6 + count * 3) {
					failure = "jpeg: malformed frame header";
					return false;
				}

				components.clear();
				for (size_t index = 0; index < count; index++) {
					Component component;
					component.Identifier = static_cast<uint8_t>(segment[6 + index * 3]);
					const uint8_t sampling = static_cast<uint8_t>(segment[7 + index * 3]);
					component.HorizontalSampling = sampling >> 4;
					component.VerticalSampling = sampling & 0x0F;
					component.QuantisationTable = static_cast<uint8_t>(segment[8 + index * 3]);

					if (component.HorizontalSampling == 0 || component.HorizontalSampling > 4 ||
						component.VerticalSampling == 0 || component.VerticalSampling > 4 ||
						component.QuantisationTable > 3) {
						failure = "jpeg: unsupported sampling factors";
						return false;
					}
					components.push_back(component);
				}
				haveFrame = true;
				break;
			}
			case 0xC2:
				failure = "jpeg: progressive images are not supported";
				return false;
			case 0xC9:
			case 0xCA:
			case 0xCB:
				failure = "jpeg: arithmetic coding is not supported";
				return false;
			case 0xC4: {
				size_t cursor = 0;
				while (cursor + 17 <= segment.size()) {
					const uint8_t identifier = static_cast<uint8_t>(segment[cursor]);
					const uint8_t slot = identifier & 0x0F;
					const bool alternating = (identifier >> 4) != 0;
					if (slot > 3) {
						failure = "jpeg: huffman table index out of range";
						return false;
					}

					HuffmanTable table;
					table.Present = true;
					size_t total = 0;
					int32_t code = 0;
					for (int bitLength = 1; bitLength <= 16; bitLength++) {
						const size_t count = static_cast<uint8_t>(segment[cursor + bitLength]);
						table.Offset[bitLength] = static_cast<int32_t>(total);
						table.Minimum[bitLength] = code;
						table.Maximum[bitLength] = count == 0 ? -1 : code + static_cast<int32_t>(count) - 1;
						code += static_cast<int32_t>(count);
						total += count;
						code <<= 1;
					}

					if (cursor + 17 + total > segment.size()) {
						failure = "jpeg: huffman table runs past its segment";
						return false;
					}
					table.Values.assign(
						reinterpret_cast<const uint8_t *>(segment.data()) + cursor + 17,
						reinterpret_cast<const uint8_t *>(segment.data()) + cursor + 17 + total
					);

					(alternating ? acTables : dcTables)[slot] = std::move(table);
					cursor += 17 + total;
				}
				break;
			}
			case 0xDB: {
				size_t cursor = 0;
				while (cursor < segment.size()) {
					const uint8_t identifier = static_cast<uint8_t>(segment[cursor++]);
					const uint8_t slot = identifier & 0x0F;
					const bool sixteenBit = (identifier >> 4) != 0;
					if (slot > 3) {
						failure = "jpeg: quantisation table index out of range";
						return false;
					}
					const size_t needed = sixteenBit ? 128 : 64;
					if (cursor + needed > segment.size()) {
						failure = "jpeg: quantisation table runs past its segment";
						return false;
					}
					for (size_t index = 0; index < 64; index++) {
						quantisation[slot][ZIGZAG[index]] =
							sixteenBit ? static_cast<uint16_t>(
											 (static_cast<uint16_t>(segment[cursor + index * 2]) << 8) |
											 static_cast<uint16_t>(segment[cursor + index * 2 + 1])
										 )
									   : static_cast<uint16_t>(segment[cursor + index]);
					}
					cursor += needed;
				}
				break;
			}
			case 0xDD:
				if (segment.size() >= 2) {
					restartInterval =
						(static_cast<uint32_t>(segment[0]) << 8) | static_cast<uint32_t>(segment[1]);
				}
				break;
			case 0xDA: {
				if (!haveFrame) {
					failure = "jpeg: scan before frame header";
					return false;
				}
				if (width == 0 || height == 0 || width > assets::Texture::MAXIMUM_DIMENSION ||
					height > assets::Texture::MAXIMUM_DIMENSION) {
					failure = "jpeg: dimensions are zero or past the ceiling";
					return false;
				}

				const size_t scanComponents = segment.empty() ? 0 : static_cast<uint8_t>(segment[0]);
				if (scanComponents != components.size() || segment.size() < 1 + scanComponents * 2) {
					// A scan covering fewer components than the frame is the
					// shape a progressive file has; a baseline one is a single
					// interleaved scan over all of them.
					failure = "jpeg: only single interleaved scans are supported";
					return false;
				}

				for (size_t index = 0; index < scanComponents; index++) {
					const uint8_t identifier = static_cast<uint8_t>(segment[1 + index * 2]);
					const uint8_t tables = static_cast<uint8_t>(segment[2 + index * 2]);

					const auto match = std::find_if(
						components.begin(), components.end(), [identifier](const Component &component) {
							return component.Identifier == identifier;
						}
					);
					if (match == components.end() || (tables >> 4) > 3 || (tables & 0x0F) > 3) {
						failure = "jpeg: scan names a component the frame does not";
						return false;
					}
					match->DcTable = tables >> 4;
					match->AcTable = tables & 0x0F;
				}

				uint32_t maximumHorizontal = 1;
				uint32_t maximumVertical = 1;
				for (const Component &component : components) {
					maximumHorizontal = std::max<uint32_t>(maximumHorizontal, component.HorizontalSampling);
					maximumVertical = std::max<uint32_t>(maximumVertical, component.VerticalSampling);
				}

				const uint32_t blocksAcross = (width + maximumHorizontal * 8 - 1) / (maximumHorizontal * 8);
				const uint32_t blocksDown = (height + maximumVertical * 8 - 1) / (maximumVertical * 8);

				for (Component &component : components) {
					component.PlaneWidth = blocksAcross * component.HorizontalSampling * 8;
					component.PlaneHeight = blocksDown * component.VerticalSampling * 8;
					component.Plane.assign(
						static_cast<size_t>(component.PlaneWidth) * component.PlaneHeight, 0
					);
					component.Predictor = 0;
				}

				BitReader reader(bytes, offset + length);
				bool failed = false;
				uint32_t sinceRestart = 0;

				for (uint32_t row = 0; row < blocksDown && !failed; row++) {
					for (uint32_t column = 0; column < blocksAcross && !failed; column++) {
						if (restartInterval != 0 && sinceRestart == restartInterval) {
							// A restart resets the DC predictors and realigns
							// to a byte, which is the whole point of it: a
							// corrupt stream cannot desynchronise past one
							// interval.
							reader.Align();
							size_t position = reader.Position();
							while (position + 1 < bytes.size()) {
								const uint8_t first = static_cast<uint8_t>(bytes[position]);
								const uint8_t second = static_cast<uint8_t>(bytes[position + 1]);
								if (first == 0xFF && second >= 0xD0 && second <= 0xD7) {
									reader.Seek(position + 2);
									break;
								}
								position++;
							}
							for (Component &component : components) {
								component.Predictor = 0;
							}
							sinceRestart = 0;
						}

						for (Component &component : components) {
							for (uint32_t vertical = 0; vertical < component.VerticalSampling; vertical++) {
								for (uint32_t horizontal = 0; horizontal < component.HorizontalSampling;
									 horizontal++) {
									const HuffmanTable &dc = dcTables[component.DcTable];
									const HuffmanTable &ac = acTables[component.AcTable];
									if (!dc.Present || !ac.Present) {
										failure = "jpeg: scan names a huffman table the file does not define";
										return false;
									}

									std::array<int32_t, 64> block{};

									const int32_t magnitude = DecodeHuffman(reader, dc, failed);
									if (failed || magnitude > 16) {
										failed = true;
										break;
									}
									component.Predictor += Extend(
										reader.Bits(static_cast<int>(magnitude)), static_cast<int>(magnitude)
									);
									block[0] =
										component.Predictor *
										static_cast<int32_t>(quantisation[component.QuantisationTable][0]);

									for (int index = 1; index < 64;) {
										const int32_t symbol = DecodeHuffman(reader, ac, failed);
										if (failed) {
											break;
										}
										const int32_t run = symbol >> 4;
										const int32_t size = symbol & 0x0F;

										if (size == 0) {
											if (run != 15) {
												break; // end of block
											}
											index += 16;
											continue;
										}

										index += run;
										if (index > 63) {
											failed = true;
											break;
										}
										const uint8_t position = ZIGZAG[index];
										block[position] =
											Extend(
												reader.Bits(static_cast<int>(size)), static_cast<int>(size)
											) *
											static_cast<int32_t>(
												quantisation[component.QuantisationTable][position]
											);
										index++;
									}

									std::array<uint8_t, 64> pixels{};
									InverseTransform(block, pixels);

									const uint32_t originX =
										(column * component.HorizontalSampling + horizontal) * 8;
									const uint32_t originY =
										(row * component.VerticalSampling + vertical) * 8;
									for (uint32_t y = 0; y < 8; y++) {
										std::memcpy(
											component.Plane.data() +
												static_cast<size_t>(originY + y) * component.PlaneWidth +
												originX,
											pixels.data() + y * 8,
											8
										);
									}
								}
								if (failed) {
									break;
								}
							}
							if (failed) {
								break;
							}
						}
						sinceRestart++;
					}
				}

				if (failed) {
					failure = "jpeg: malformed entropy-coded data";
					return false;
				}

				// Each component brought up to the image's resolution, so the
				// colour conversion below is one lookup per channel rather than
				// a sampling decision repeated three times.
				std::vector<std::vector<uint8_t>> planes;
				planes.reserve(components.size());
				for (const Component &component : components) {
					planes.push_back(Upsample(component, width, height, maximumHorizontal, maximumVertical));
				}

				assets::TextureData decoded;
				decoded.Width = width;
				decoded.Height = height;
				decoded.Format = assets::TextureFormat::RGBA8;
				decoded.Pixels.resize(static_cast<size_t>(width) * height * 4);

				for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; pixel++) {
					int red = 0, green = 0, blue = 0;
					if (planes.size() == 1) {
						red = green = blue = planes[0][pixel];
					} else {
						const float luma = static_cast<float>(planes[0][pixel]);
						const float blueChroma = static_cast<float>(planes[1][pixel]) - 128.0f;
						const float redChroma = static_cast<float>(planes[2][pixel]) - 128.0f;

						red = static_cast<int>(std::lround(luma + 1.402f * redChroma));
						green = static_cast<int>(
							std::lround(luma - 0.344136f * blueChroma - 0.714136f * redChroma)
						);
						blue = static_cast<int>(std::lround(luma + 1.772f * blueChroma));
					}

					decoded.Pixels[pixel * 4] = static_cast<std::byte>(std::clamp(red, 0, 255));
					decoded.Pixels[pixel * 4 + 1] = static_cast<std::byte>(std::clamp(green, 0, 255));
					decoded.Pixels[pixel * 4 + 2] = static_cast<std::byte>(std::clamp(blue, 0, 255));
					decoded.Pixels[pixel * 4 + 3] = std::byte{255};
				}

				out = std::move(decoded);
				return true;
			}
			default:
				break;
			}

			offset += length;
		}

		failure = "jpeg: no scan";
		return false;
	}
}
