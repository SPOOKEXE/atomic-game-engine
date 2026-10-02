#include <engine/bake/GifWrite.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <string_view>
#include <utility>

namespace engine::bake {
	namespace {
		struct Colour {
			uint32_t Rgb = 0, Count = 0;
			uint8_t Index = 0;
		};
		struct Box {
			size_t First = 0, Last = 0;
			unsigned Axis = 0, Range = 0;
		};
		uint8_t Channel(uint32_t rgb, unsigned axis) {
			return static_cast<uint8_t>(rgb >> (axis * 8));
		}
		uint32_t Pixel(const uint8_t *rgba, uint8_t bits) {
			uint32_t rgb = 0;
			const unsigned levels = (1u << bits) - 1;
			for (unsigned axis = 0; axis < 3; ++axis) {
				const unsigned quantized = (rgba[axis] * levels + 127) / 255;
				rgb |= ((quantized * 255 + levels / 2) / levels) << (axis * 8);
			}
			return rgb;
		}
		std::array<uint32_t, 256> Palette(std::vector<Colour> &colours) {
			std::array<uint32_t, 256> palette{};
			std::array<Box, 255> boxes{};
			size_t count = colours.empty() ? 0 : 1;
			const auto measure = [&](Box &box) {
				if (box.First == box.Last) return;
				std::array<unsigned, 3> low{255, 255, 255}, high{};
				for (size_t i = box.First; i < box.Last; ++i)
					for (unsigned axis = 0; axis < 3; ++axis) {
						const unsigned value = Channel(colours[i].Rgb, axis);
						low[axis] = std::min(low[axis], value);
						high[axis] = std::max(high[axis], value);
					}
				for (unsigned axis = 0; axis < 3; ++axis)
					if (high[axis] - low[axis] > box.Range) {
						box.Range = high[axis] - low[axis];
						box.Axis = axis;
					}
			};
			boxes[0] = {0, colours.size()};
			measure(boxes[0]);
			while (count < 255) {
				size_t selected = count;
				unsigned axis = 0, greatest = 0;
				for (size_t box = 0; box < count; ++box)
					if (boxes[box].Range > greatest) {
						selected = box;
						greatest = boxes[box].Range;
						axis = boxes[box].Axis;
					}
				if (selected == count) break;
				const Box box = boxes[selected];
				std::sort(
					colours.begin() + box.First,
					colours.begin() + box.Last,
					[axis](const Colour &a, const Colour &b) {
						const auto ac = Channel(a.Rgb, axis), bc = Channel(b.Rgb, axis);
						return ac == bc ? a.Rgb < b.Rgb : ac < bc;
					}
				);
				uint64_t total = 0, accumulated = 0;
				for (size_t i = box.First; i < box.Last; ++i)
					total += colours[i].Count;
				size_t split = box.First;
				do {
					accumulated += colours[split++].Count;
				} while (split + 1 < box.Last && accumulated * 2 < total);
				boxes[selected] = {box.First, split};
				boxes[count] = {split, box.Last};
				measure(boxes[selected]);
				measure(boxes[count++]);
			}
			for (size_t box = 0; box < count; ++box) {
				uint64_t total = 0;
				std::array<uint64_t, 3> sums{};
				for (size_t i = boxes[box].First; i < boxes[box].Last; ++i) {
					colours[i].Index = static_cast<uint8_t>(box + 1);
					total += colours[i].Count;
					for (unsigned axis = 0; axis < 3; ++axis)
						sums[axis] += static_cast<uint64_t>(Channel(colours[i].Rgb, axis)) * colours[i].Count;
				}
				for (unsigned axis = 0; axis < 3; ++axis)
					palette[box + 1] |= static_cast<uint32_t>((sums[axis] + total / 2) / total) << (axis * 8);
			}
			std::sort(colours.begin(), colours.end(), [](const Colour &a, const Colour &b) {
				return a.Rgb < b.Rgb;
			});
			return palette;
		}
	}
	bool WriteGif(
		std::span<const GifFrame> frames,
		uint8_t quality,
		uint64_t maximumBytes,
		std::vector<std::byte> &output,
		std::string &failure,
		uint16_t loopCount
	) try {
		const auto fail = [&](const char *reason) {
			failure = reason;
			return false;
		};
		if (frames.empty() || frames.size() > 4096 || quality > 3)
			return fail("GIF requires 1..4096 frames and quality 0..3");
		uint64_t pixels = 0, largest = 0, outputBound = 33;
		for (const auto &frame : frames) {
			if (!frame.Width || !frame.Height || frame.Width > 65535 || frame.Height > 65535 ||
				frame.Width != frames.front().Width || frame.Height != frames.front().Height)
				return fail("GIF frames need equal nonzero 16-bit dimensions");
			const uint64_t area = static_cast<uint64_t>(frame.Width) * frame.Height;
			if (area > 16 * 1024 * 1024 || frame.Rgba.size() != area * 4 || !frame.DelayCentiseconds)
				return fail("GIF frame pixels or delay are invalid");
			pixels += area;
			largest = std::max(largest, area);
			// Literal LZW codes with a clear every 200 pixels keep all codes at nine bits.
			const uint64_t packed = ((area + (area + 199) / 200 + 1) * 9 + 7) / 8;
			outputBound += 790 + packed + (packed + 254) / 255;
		}
		if (pixels > 16 * 1024 * 1024 || outputBound > maximumBytes ||
			largest * (sizeof(uint32_t) + sizeof(Colour)) > maximumBytes - outputBound)
			return fail("GIF encoding work or owned byte budget exceeded");
		std::vector<std::byte> result;
		result.reserve(static_cast<size_t>(outputBound));
		const auto byte = [&](unsigned value) { result.push_back(static_cast<std::byte>(value & 255)); };
		const auto shortValue = [&](uint32_t value) {
			byte(value);
			byte(value >> 8);
		};
		for (char value : std::string_view{"GIF89a"})
			byte(static_cast<unsigned char>(value));
		shortValue(frames.front().Width);
		shortValue(frames.front().Height);
		byte(0x70);
		byte(0);
		byte(0);
		byte(0x21);
		byte(0xff);
		byte(11);
		for (char value : std::string_view{"NETSCAPE2.0"})
			byte(static_cast<unsigned char>(value));
		byte(3);
		byte(1);
		shortValue(loopCount);
		byte(0);
		const uint8_t bits = std::array<uint8_t, 4>{2, 4, 6, 8}[quality];
		for (const auto &frame : frames) {
			const size_t area = static_cast<size_t>(frame.Width) * frame.Height;
			std::vector<uint32_t> keys;
			keys.reserve(area);
			for (size_t i = 0; i < area; ++i)
				if (frame.Rgba[i * 4 + 3] >= 128) keys.push_back(Pixel(frame.Rgba.data() + i * 4, bits));
			std::sort(keys.begin(), keys.end());
			std::vector<Colour> colours;
			colours.reserve(keys.size());
			for (uint32_t key : keys) {
				if (!colours.empty() && colours.back().Rgb == key)
					++colours.back().Count;
				else
					colours.push_back({key, 1, 0});
			}
			const auto palette = Palette(colours);
			byte(0x21);
			byte(0xf9);
			byte(4);
			byte(9);
			shortValue(frame.DelayCentiseconds);
			byte(0);
			byte(0);
			byte(0x2c);
			shortValue(0);
			shortValue(0);
			shortValue(frame.Width);
			shortValue(frame.Height);
			byte(0x87);
			for (uint32_t rgb : palette)
				for (unsigned axis = 0; axis < 3; ++axis)
					byte(Channel(rgb, axis));
			byte(8);
			std::array<uint8_t, 255> block{};
			size_t used = 0;
			uint32_t pending = 0;
			unsigned pendingBits = 0;
			const auto flush = [&] {
				byte(static_cast<unsigned>(used));
				for (size_t i = 0; i < used; ++i)
					byte(block[i]);
				used = 0;
			};
			const auto code = [&](uint16_t value) {
				pending |= static_cast<uint32_t>(value) << pendingBits;
				pendingBits += 9;
				while (pendingBits >= 8) {
					block[used++] = static_cast<uint8_t>(pending);
					pending >>= 8;
					pendingBits -= 8;
					if (used == block.size()) flush();
				}
			};
			for (size_t i = 0; i < area; ++i) {
				if (i % 200 == 0) code(256);
				uint8_t index = 0;
				if (frame.Rgba[i * 4 + 3] >= 128) {
					const uint32_t key = Pixel(frame.Rgba.data() + i * 4, bits);
					index = std::lower_bound(
								colours.begin(), colours.end(), key, [](const Colour &colour, uint32_t rgb) {
									return colour.Rgb < rgb;
								}
					)->Index;
				}
				code(index);
			}
			code(257);
			if (pendingBits) {
				block[used++] = static_cast<uint8_t>(pending);
				if (used == block.size()) flush();
			}
			if (used) flush();
			byte(0);
		}
		byte(0x3b);
		output = std::move(result);
		return true;
	} catch (const std::bad_alloc &) {
		failure = "GIF encoding allocation failed";
		return false;
	}
}
