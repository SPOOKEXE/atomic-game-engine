#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fixtures {
	inline void Big32(std::vector<std::byte> &out, uint32_t value) {
		for (int shift = 24; shift >= 0; shift -= 8)
			out.push_back(static_cast<std::byte>(value >> shift));
	}
	inline uint32_t Crc(std::span<const std::byte> bytes) {
		uint32_t value = 0xffffffff;
		for (std::byte byte : bytes) {
			value ^= static_cast<uint8_t>(byte);
			for (int bit = 0; bit < 8; bit++)
				value = (value >> 1) ^ ((value & 1) ? 0xedb88320 : 0);
		}
		return value ^ 0xffffffff;
	}
	inline void
	Chunk(std::vector<std::byte> &out, std::span<const std::byte> type, std::span<const std::byte> data) {
		Big32(out, static_cast<uint32_t>(data.size()));
		const size_t begin = out.size();
		out.insert(out.end(), type.begin(), type.end());
		out.insert(out.end(), data.begin(), data.end());
		Big32(out, Crc(std::span(out).subspan(begin)));
	}
	// grug build flat grey fixtures outside measured work. stored DEFLATE keeps
	// fixture independent of decoder vendor, no test encoder hiding filter bugs.
	inline std::vector<std::byte> GreyPng(uint32_t width, uint32_t height) {
		std::vector<std::byte> raw(static_cast<size_t>(width + 1) * height, std::byte{128});
		for (uint32_t y = 0; y < height; y++)
			raw[static_cast<size_t>(y) * (width + 1)] = std::byte{0};
		std::vector<std::byte> compressed{std::byte{0x78}, std::byte{0x01}};
		for (size_t offset = 0; offset < raw.size();) {
			const size_t count = std::min<size_t>(65535, raw.size() - offset);
			compressed.push_back(static_cast<std::byte>(offset + count == raw.size() ? 1 : 0));
			compressed.push_back(static_cast<std::byte>(count));
			compressed.push_back(static_cast<std::byte>(count >> 8));
			compressed.push_back(static_cast<std::byte>(~count));
			compressed.push_back(static_cast<std::byte>((~count) >> 8));
			compressed.insert(compressed.end(), raw.begin() + offset, raw.begin() + offset + count);
			offset += count;
		}
		uint32_t first = 1, second = 0;
		for (std::byte byte : raw) {
			first = (first + static_cast<uint8_t>(byte)) % 65521;
			second = (second + first) % 65521;
		}
		Big32(compressed, (second << 16) | first);
		std::vector<std::byte> png{
			std::byte{0x89},
			std::byte{'P'},
			std::byte{'N'},
			std::byte{'G'},
			std::byte{13},
			std::byte{10},
			std::byte{26},
			std::byte{10}
		};
		std::vector<std::byte> header;
		Big32(header, width);
		Big32(header, height);
		header.insert(header.end(), {std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
		const std::byte ihdr[]{std::byte{'I'}, std::byte{'H'}, std::byte{'D'}, std::byte{'R'}};
		const std::byte idat[]{std::byte{'I'}, std::byte{'D'}, std::byte{'A'}, std::byte{'T'}};
		const std::byte iend[]{std::byte{'I'}, std::byte{'E'}, std::byte{'N'}, std::byte{'D'}};
		Chunk(png, ihdr, header);
		Chunk(png, idat, compressed);
		Chunk(png, iend, {});
		return png;
	}
	inline void Segment(std::vector<std::byte> &out, uint8_t marker, std::span<const std::byte> data) {
		out.push_back(std::byte{0xff});
		out.push_back(static_cast<std::byte>(marker));
		out.push_back(static_cast<std::byte>((data.size() + 2) >> 8));
		out.push_back(static_cast<std::byte>(data.size() + 2));
		out.insert(out.end(), data.begin(), data.end());
	}
	// grug single zero DC and end-of-block symbol give grey 128 in every block.
	inline std::vector<std::byte> GreyJpeg(uint16_t width, uint16_t height) {
		std::vector<std::byte> jpeg{std::byte{0xff}, std::byte{0xd8}};
		std::vector<std::byte> quant(65, std::byte{1});
		quant[0] = std::byte{0};
		Segment(jpeg, 0xdb, quant);
		const std::byte frame[]{
			std::byte{8},
			static_cast<std::byte>(height >> 8),
			static_cast<std::byte>(height),
			static_cast<std::byte>(width >> 8),
			static_cast<std::byte>(width),
			std::byte{1},
			std::byte{1},
			std::byte{0x11},
			std::byte{0}
		};
		Segment(jpeg, 0xc0, frame);
		std::vector<std::byte> tables;
		for (uint8_t kind : {uint8_t{0}, uint8_t{0x10}}) {
			tables.push_back(static_cast<std::byte>(kind));
			tables.push_back(std::byte{1});
			tables.insert(tables.end(), 15, std::byte{0});
			tables.push_back(std::byte{0});
		}
		Segment(jpeg, 0xc4, tables);
		const std::byte scan[]{
			std::byte{1}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{63}, std::byte{0}
		};
		Segment(jpeg, 0xda, scan);
		const size_t bits = static_cast<size_t>((width + 7) / 8) * ((height + 7) / 8) * 2;
		jpeg.insert(jpeg.end(), (bits + 7) / 8, std::byte{0});
		if (bits % 8 != 0) jpeg.back() = static_cast<std::byte>((1 << (8 - bits % 8)) - 1);
		jpeg.insert(jpeg.end(), {std::byte{0xff}, std::byte{0xd9}});
		return jpeg;
	}
}
