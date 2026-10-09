#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagecodec {
	// grug keep pixels owned and top row first. png/jpeg produce encoded sRGB,
	// straight alpha RGBA8. raw bytes keep caller's chosen colour space.
	struct Image {
		// grug columns in each top-first row.
		uint32_t Width = 0;
		// grug number of rows.
		uint32_t Height = 0;
		// grug exactly Width * Height * 4 straight alpha bytes.
		std::vector<std::byte> Pixels;
	};

	// grug check these before allocating pixels or copying encoded bytes.
	struct Limits {
		// grug refuse wider headers before pixel allocation.
		uint32_t MaximumWidth = 1920;
		// grug refuse taller headers before pixel allocation.
		uint32_t MaximumHeight = 1080;
		// grug cap raw bytes or complete compressed file before parser work.
		size_t MaximumEncodedBytes = 8294400;
		// grug cap expanded RGBA8; temporary decode rows stay bounded by extent.
		size_t MaximumPixelBytes = 8294400;
	};

	// grug choose explicit compressed file format, no guessing arbitrary bytes.
	enum class Format : uint8_t {
		// grug noninterlaced 8/16-bit PNG.
		Png,
		// grug baseline/sequential 8-bit grey or YCbCr JPEG.
		Jpeg
	};

	// grug decode supported still image. failure leave out alone.
	bool Decode(
		std::span<const std::byte> bytes,
		Format format,
		Image &out,
		std::string &failure,
		const Limits &limits = {}
	);

	// grug copy exact width * height * 4 bytes. no colour conversion here.
	bool DecodeRaw(
		uint32_t width,
		uint32_t height,
		std::span<const std::byte> bytes,
		Image &out,
		std::string &failure,
		const Limits &limits = {}
	);

	// grug accept canonical padded base64 only. no whitespace or data URI.
	// inspect whole input and decoded count before allocation. failure keep out.
	bool DecodeBase64(
		std::string_view encoded,
		std::vector<std::byte> &out,
		std::string &failure,
		size_t maximumBytes = 8294400
	);

	// grug encode bounded bytes with standard alphabet and canonical padding.
	bool EncodeBase64(
		std::span<const std::byte> bytes,
		std::string &out,
		std::string &failure,
		size_t maximumBytes = 8294400
	);
}
