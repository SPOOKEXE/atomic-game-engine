#include "StillExport.hpp"

#include "IconExport.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <sstream>

namespace engine::imagegraphexport::runner {
	namespace {
		void Little(std::vector<uint8_t> &bytes, uint64_t value, unsigned count) {
			for (unsigned index = 0; index < count; index++)
				bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
		}
		void Text(std::vector<uint8_t> &bytes, std::string_view text) {
			bytes.insert(bytes.end(), text.begin(), text.end());
			bytes.push_back(0);
		}
		void Float(std::vector<uint8_t> &bytes, float value) {
			Little(bytes, std::bit_cast<uint32_t>(value), 4);
		}
		void Attribute(
			std::vector<uint8_t> &bytes,
			std::string_view name,
			std::string_view type,
			const std::vector<uint8_t> &data
		) {
			Text(bytes, name);
			Text(bytes, type);
			Little(bytes, data.size(), 4);
			bytes.insert(bytes.end(), data.begin(), data.end());
		}
	}

	bool EncodeStill(
		std::string_view extension,
		const engine::imagegraph::Image &image,
		std::vector<uint8_t> &bytes,
		std::string &failure
	) {
		using namespace engine::imagegraph;
		bytes.clear();
		if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
			!FiniteSurfaceSamples(image)) {
			failure = "evaluated image violates export bounds";
			return false;
		}
		if (extension == ".ico") return EncodeIcon(image, bytes, failure);
		if (extension == ".bmp") {
			const uint64_t rowBytes = (static_cast<uint64_t>(image.Width) * 3 + 3) & ~uint64_t{3};
			const uint64_t size = 54 + rowBytes * image.Height;
			if (size > Limits::MaximumEvaluationBytes || size > std::numeric_limits<uint32_t>::max()) {
				failure = "BMP export exceeds its byte budget";
				return false;
			}
			bytes.reserve(static_cast<size_t>(size));
			bytes.insert(bytes.end(), {'B', 'M'});
			Little(bytes, size, 4);
			Little(bytes, 0, 4);
			Little(bytes, 54, 4);
			Little(bytes, 40, 4);
			Little(bytes, image.Width, 4);
			Little(bytes, image.Height, 4);
			Little(bytes, 1, 2);
			Little(bytes, 24, 2);
			Little(bytes, 0, 4);
			Little(bytes, rowBytes * image.Height, 4);
			Little(bytes, 18, 4);
			Little(bytes, 18, 4);
			Little(bytes, 0, 4);
			Little(bytes, 0, 4);
			for (uint32_t row = 0; row < image.Height; row++) {
				for (uint32_t x = 0; x < image.Width; x++) {
					SurfacePixel pixel{};
					if (!LoadSurfacePixel(image, x, image.Height - row - 1, pixel)) return false;
					// BMP has no alpha. Source exporter composites onto black before writing BGR.
					for (unsigned channel : {2u, 1u, 0u}) {
						const double sample =
							std::clamp(pixel[channel], 0.0, 1.0) * std::clamp(pixel[3], 0.0, 1.0);
						bytes.push_back(static_cast<uint8_t>(sample * 255.0));
					}
				}
				bytes.insert(bytes.end(), static_cast<size_t>(rowBytes - image.Width * 3), 0);
			}
		} else if (extension == ".exr") {
			const auto description = DescribeSurfaceFormat(image.Format);
			const unsigned channelCount = description->Channels;
			const uint64_t pixelBytes = static_cast<uint64_t>(image.Width) * image.Height * channelCount * 4;
			if (pixelBytes + static_cast<uint64_t>(image.Height) * 16 + 4096 >
				Limits::MaximumEvaluationBytes) {
				failure = "EXR export exceeds its byte budget";
				return false;
			}
			Little(bytes, 20000630, 4);
			Little(bytes, 2, 4);
			std::vector<uint8_t> data;
			const std::array<std::string_view, 4> names = {"A", "B", "G", "R"};
			for (unsigned channel = 0; channel < channelCount; channel++) {
				Text(data, channelCount == 1 ? "Y" : names[channel]);
				Little(data, 2, 4);
				Little(data, 0, 4);
				Little(data, 1, 4);
				Little(data, 1, 4);
			}
			data.push_back(0);
			Attribute(bytes, "channels", "chlist", data);
			Attribute(bytes, "compression", "compression", {0});
			data.clear();
			Little(data, 0, 8);
			Little(data, image.Width - 1, 4);
			Little(data, image.Height - 1, 4);
			Attribute(bytes, "dataWindow", "box2i", data);
			Attribute(bytes, "displayWindow", "box2i", data);
			Attribute(bytes, "lineOrder", "lineOrder", {0});
			data.clear();
			Float(data, 1);
			Attribute(bytes, "pixelAspectRatio", "float", data);
			data.clear();
			Float(data, 0);
			Float(data, 0);
			Attribute(bytes, "screenWindowCenter", "v2f", data);
			data.clear();
			Float(data, 1);
			Attribute(bytes, "screenWindowWidth", "float", data);
			bytes.push_back(0);
			const uint64_t firstScanline = bytes.size() + static_cast<uint64_t>(image.Height) * 8;
			const uint64_t scanlineBytes = static_cast<uint64_t>(image.Width) * channelCount * 4;
			for (uint32_t row = 0; row < image.Height; row++)
				Little(bytes, firstScanline + row * (scanlineBytes + 8), 8);
			for (uint32_t row = 0; row < image.Height; row++) {
				Little(bytes, row, 4);
				Little(bytes, scanlineBytes, 4);
				for (unsigned channel = 0; channel < channelCount; channel++) {
					for (uint32_t x = 0; x < image.Width; x++) {
						SurfacePixel pixel{};
						if (!LoadSurfacePixel(image, x, row, pixel)) return false;
						Float(bytes, static_cast<float>(pixel[channelCount == 1 ? 0 : 3 - channel]));
					}
				}
			}
		} else {
			failure = "unsupported native still export format";
			return false;
		}
		return true;
	}
}
