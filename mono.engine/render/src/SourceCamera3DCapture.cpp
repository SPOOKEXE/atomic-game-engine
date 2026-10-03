#include "SourceCamera3DCapture.hpp"

#include "TextureFormatSupport.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <algorithm>
#include <cstring>
#include <new>
namespace engine::render::imagegraph {
	bool CompleteSourceCamera3DCapture(
		const SourceCamera3DRequest &request,
		std::span<const std::span<const std::byte>> downloads,
		uint64_t maximumBytes,
		engine::imagegraph::HostNodeCapture &receipt
	) try {
		ENGINE_PROFILE("source camera completed host surfaces");
		using namespace engine::imagegraph;
		if (downloads.size() != 7 || request.Width == 0 || request.Height == 0) return false;
		SurfaceFormat format;
		using F = assets::TextureFormat;
		switch (request.Format) {
		case F::RGBA8:
		case F::RGBA8_LINEAR:
			format = SurfaceFormat::RGBA8Unorm;
			break;
		case F::RGBA4_UNORM:
		case F::RGBA4_SRGB:
			format = SurfaceFormat::RGBA4Unorm;
			break;
		case F::RGBA16_FLOAT:
			format = SurfaceFormat::RGBA16Float;
			break;
		case F::RGBA32_FLOAT:
			format = SurfaceFormat::RGBA32Float;
			break;
		case F::R8:
			format = SurfaceFormat::R8Unorm;
			break;
		case F::R16_FLOAT:
			format = SurfaceFormat::R16Float;
			break;
		case F::R32_FLOAT:
			format = SurfaceFormat::R32Float;
			break;
		default:
			return false;
		}
		const auto layout = CheckedSurfaceLayout(request.Width, request.Height, format, maximumBytes);
		const auto previous = HostCaptureRetainedPayloadBytes(receipt);
		if (!layout || !previous) return false;
		const uint64_t rawBytes = uint64_t(request.Width) * request.Height *
								  (format == SurfaceFormat::RGBA4Unorm || format == SurfaceFormat::R8Unorm
									   ? 4
									   : DescribeSurfaceFormat(format)->BytesPerPixel);
		for (const auto bytes : downloads)
			if (bytes.size() != rawBytes) return false;
		const uint64_t storage = 7 * (sizeof(HostCapturedImage) + 32 + layout->Bytes);
		if (*previous > maximumBytes || storage > maximumBytes - *previous) return false;
		constexpr std::string_view ports[] = {
			"rendered", "diffuse", "normal", "view_normal", "depth", "shadow", "ambient_occlusion"
		};
		std::vector<HostCapturedImage> images;
		images.reserve(7);
		uint64_t retained = images.capacity() * sizeof(HostCapturedImage);
		for (size_t index = 0; index < 7; ++index) {
			Image image{request.Width, request.Height, {}, 0, format};
			image.Pixels.resize(layout->Bytes);
			const auto raw = downloads[index];
			if (format == SurfaceFormat::RGBA4Unorm) {
				if (!engine::render::detail::CopyRgba8ToRgba4(
						raw, {reinterpret_cast<std::byte *>(image.Pixels.data()), image.Pixels.size()}
					))
					return false;
			} else if (format == SurfaceFormat::R8Unorm) {
				for (size_t pixel = 0; pixel < image.Pixels.size(); ++pixel)
					image.Pixels[pixel] = std::to_integer<uint8_t>(raw[pixel * 4]);
			} else
				std::memcpy(image.Pixels.data(), raw.data(), raw.size());
			if (!FiniteSurfaceSamples(image)) return false;
			image.Hash = SurfaceHash(image);
			images.push_back({std::string(ports[index]), std::move(image)});
			const auto &added = images.back();
			const uint64_t bytes = added.Port.capacity() + 1 + added.Data.Pixels.capacity();
			if (bytes > maximumBytes || retained > maximumBytes - bytes) return false;
			retained += bytes;
			if (retained > maximumBytes - *previous) return false;
		}
		receipt.Images = std::move(images);
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}
}
