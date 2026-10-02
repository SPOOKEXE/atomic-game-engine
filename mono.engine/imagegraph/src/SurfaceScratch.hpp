#pragma once

// Scratch pixels stay admitted until the temporary surface is destroyed.
#include "NodeExecutors.hpp"

#include <new>
#include <optional>

namespace engine::imagegraph::detail {
	struct SurfaceScratch {
		AllocationReservation Charge;
		Image Data;
	};

	inline std::optional<SurfaceScratch> MakeSurfaceScratch(
		NodeContext &context, uint32_t width, uint32_t height, SurfaceFormat format, std::string_view port
	) {
		const auto cap = context.Request.MaximumImageDimension;
		if (!cap || cap > Limits::MaximumDimension) {
			context.Fail(Status::InvalidValue, "image dimension cap is invalid", port);
			return std::nullopt;
		}
		if (width > cap || height > cap) {
			context.Fail(Status::LimitExceeded, "scratch surface exceeds the image dimension cap", port);
			return std::nullopt;
		}
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumOutputBytes);
		if (!layout) {
			context.Fail(Status::LimitExceeded, "scratch surface layout exceeds its limit", port);
			return std::nullopt;
		}
		auto charge = context.ReserveWorkspace(layout->Bytes, port);
		if (!charge) return std::nullopt;
		SurfaceScratch result{std::move(*charge), Image{width, height, {}, 0, format}};
		try {
			ENGINE_PROFILE("imagegraph.surface.scratch");
			result.Data.Pixels.resize(static_cast<size_t>(layout->Bytes));
			core::Metrics::Count("imagegraph.surface.scratch.allocated_payload_bytes", layout->Bytes);
			core::Metrics::Count("imagegraph.surface.scratch.allocations", 1);
		} catch (const std::bad_alloc &) {
			context.Fail(Status::LimitExceeded, "scratch surface allocation failed", port);
			return std::nullopt;
		}
		return result;
	}
	inline bool
	CopySurfaceSamples(NodeContext &context, const Image &source, Image &target, std::string_view port) {
		if (source.Width != target.Width || source.Height != target.Height)
			return context.Fail(Status::InvalidValue, "surface conversion dimensions differ", port);
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				SurfacePixel pixel;
				if (!LoadSurfacePixel(source, x, y, pixel) || !StoreSurfacePixel(target, x, y, pixel))
					return context.Fail(
						Status::InvalidValue, "surface conversion exceeds numeric range", port
					);
			}
		return true;
	}

}
