#include "SourceSdf.hpp"
#include "TextureFormatSupport.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <cstring>
#include <new>
namespace engine::render::imagegraph {
	bool CompleteSourceSdfCapture(
		const SourceSdfRequest &request,
		std::span<const std::byte> download,
		uint64_t maximumBytes,
		engine::imagegraph::HostNodeCapture &receipt,
		bool packed
	) try {
		ENGINE_PROFILE("source sdf completed host surface");
		using namespace engine::imagegraph;
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
		const auto support = detail::TextureFormatForUpload(request.Format);
		if (!layout || !previous || !support ||
			download.size() !=
				uint64_t(request.Width) * request.Height *
					(packed ? DescribeSurfaceFormat(format)->BytesPerPixel : support->UploadBytesPerPixel))
			return false;
		const uint64_t storage = sizeof(HostCapturedImage) + 32 + layout->Bytes;
		if (*previous > maximumBytes || storage > maximumBytes - *previous) return false;
		Image image{request.Width, request.Height, {}, 0, format};
		image.Pixels.resize(layout->Bytes);
		if (!packed && format == SurfaceFormat::RGBA4Unorm) {
			if (!detail::CopyRgba8ToRgba4(
					download, {reinterpret_cast<std::byte *>(image.Pixels.data()), image.Pixels.size()}
				))
				return false;
		} else if (!packed && format == SurfaceFormat::R8Unorm) {
			for (size_t pixel = 0; pixel < image.Pixels.size(); ++pixel)
				image.Pixels[pixel] = std::to_integer<uint8_t>(download[pixel * 4]);
		} else
			std::memcpy(image.Pixels.data(), download.data(), download.size());
		if (!FiniteSurfaceSamples(image)) return false;
		image.Hash = SurfaceHash(image);
		std::vector<HostCapturedImage> images;
		images.reserve(1);
		images.push_back({"surface_out", std::move(image)});
		const uint64_t actual = images.capacity() * sizeof(HostCapturedImage) + images[0].Port.capacity() +
								1 + images[0].Data.Pixels.capacity();
		if (actual > maximumBytes - *previous) return false;
		receipt.Images = std::move(images);
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	}
}
