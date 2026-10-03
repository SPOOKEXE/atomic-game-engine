#pragma once

#include "../SourceSafeDraw.hpp"
#include "Source2DReferenceUnits.hpp"

namespace engine::imagegraph::detail::source2d {
	constexpr uint64_t COMPLEX_GENERATOR_WORK_LIMIT = 64000000;
	// A work unit is a scalar read, arithmetic/comparison, or math-function call.
	// These include both typed writes, optional UV/mask reads and mapped control reads.
	constexpr uint64_t JULIA_PIXEL_BASE_WORK = 512, JULIA_ITERATION_WORK = 16;
	constexpr uint64_t GABOR_PIXEL_WORK = 256 + 25 * 96;
	// Quote the largest supported typed pixel before later rows select their formats.
	constexpr uint64_t COMPLEX_GENERATOR_PIXEL_BYTES = 16;
	constexpr uint64_t COMPLEX_GENERATOR_ROW_BYTES =
		sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
		sizeof(std::pair<std::string, ImageArray>) + sizeof(std::pair<std::string_view, SourceSocketDomain>) +
		sizeof(ElementValue) + sizeof(ImageArrayItem) + sizeof(Image) + 256;
	struct ComplexCanvas {
		Vector2 Raw{};
		uint32_t Width = 0, Height = 0;
	};
	inline bool ResolveComplexCanvas(NodeContext &c, ComplexCanvas &canvas) {
		const auto *mask = c.Input("mask");
		if (!ResolveGeneratorDimensions(c, mask, canvas.Width, canvas.Height)) return false;
		canvas.Raw = c.Vec2("dimension", {1, 1});
		if (!c.IsLinked("dimension")) {
			const auto unit = c.Integer("dimension_unit", 1);
			if (unit == 1) {
				canvas.Raw.X *= c.Project.SurfaceWidth;
				canvas.Raw.Y *= c.Project.SurfaceHeight;
			} else if (unit == 2) {
				canvas.Raw.X *= mask->Width;
				canvas.Raw.Y *= mask->Height;
			}
		}
		return (canvas.Raw.X != 0 && canvas.Raw.Y != 0) ||
			   c.Fail(Status::UnsupportedExecution, "Generator raw canvas divides by zero", "dimension");
	}
	inline bool ComplexBatchAdmission(
		NodeContext &c,
		uint64_t perPixel,
		std::string_view output,
		std::string_view workPort,
		size_t outputCount = 1
	) {
		if (c.ProcessorRow != 0) return true;
		if (!PreflightGeneratorDimensions(c)) return false;
		GeneratorDimensionBounds dimension, units;
		if (!GeneratorDimensionBoundsFor(c, "dimension", dimension) ||
			!GeneratorDimensionBoundsFor(c, "dimension_unit", units))
			return false;
		double width = dimension.HasValue ? dimension.WidthMaximum : 1;
		double height = dimension.HasValue ? dimension.HeightMaximum : 1;
		if (c.IsLinked("dimension")) {
			if (const auto *image = c.Input("dimension")) {
				width = std::max(width, double(image->Width));
				height = std::max(height, double(image->Height));
			}
			for (const auto &[port, images] : c.ImageArrays)
				if (port == "dimension" && images)
					for (const auto &image : images->Images) {
						width = std::max(width, double(image.Width));
						height = std::max(height, double(image.Height));
					}
		} else {
			const auto unit = c.Integer("dimension_unit", 1);
			if (unit < 0 || unit > 2)
				return c.Fail(Status::InvalidValue, "Dimension unit is invalid", "dimension_unit");
			const unsigned modes = units.Units ? units.Units : unsigned(1u << unit);
			double xFactor = (modes & 1) ? 1 : 0, yFactor = xFactor;
			if (modes & 2) {
				xFactor = std::max(xFactor, double(c.Project.SurfaceWidth));
				yFactor = std::max(yFactor, double(c.Project.SurfaceHeight));
			}
			if (modes & 4) {
				if (const auto *mask = c.Input("mask")) {
					xFactor = std::max(xFactor, double(mask->Width));
					yFactor = std::max(yFactor, double(mask->Height));
				}
				for (const auto &[port, images] : c.ImageArrays)
					if (port == "mask" && images)
						for (const auto &image : images->Images) {
							xFactor = std::max(xFactor, double(image.Width));
							yFactor = std::max(yFactor, double(image.Height));
						}
			}
			width *= xFactor;
			height *= yFactor;
		}
		const uint64_t w = uint64_t(std::max(1., GeneratorRoundHalfEven(width)));
		const uint64_t h = uint64_t(std::max(1., GeneratorRoundHalfEven(height)));
		const uint64_t rows = std::max<size_t>(1, c.ProcessorCount), pixels = w * h;
		if (perPixel == 0 || perPixel > COMPLEX_GENERATOR_WORK_LIMIT ||
			rows > COMPLEX_GENERATOR_WORK_LIMIT / perPixel ||
			pixels > COMPLEX_GENERATOR_WORK_LIMIT / perPixel / rows)
			return c.Fail(
				Status::LimitExceeded, "Generator whole-array work exceeds the native CPU limit", workPort
			);
		// All declared targets remain live together, including processor row publication.
		if (outputCount == 0 || outputCount > Limits::MaximumArrayElements)
			return c.Fail(Status::LimitExceeded, "Generator output count exceeds native bounds", output);
		const uint64_t rowBytes = pixels * COMPLEX_GENERATOR_PIXEL_BYTES + COMPLEX_GENERATOR_ROW_BYTES;
		if (rows > Limits::MaximumEvaluationBytes / rowBytes / outputCount)
			return c.Fail(
				Status::LimitExceeded, "Generator whole-array targets exceed native byte bounds", output
			);
		const uint64_t bytes = rows * rowBytes * outputCount;
		auto charge = c.ReserveWorkspace(bytes, output);
		return bool(charge);
	}
	inline bool ReferenceVector(NodeContext &c, std::string_view port, Vector2 raw, Vector2 &value) {
		value = c.Vec2(port);
		const auto unit = c.Integer(std::string(port) + "_unit", 1);
		if (unit < 0 || unit > 1) return c.Fail(Status::InvalidValue, "Reference unit is invalid", port);
		const auto domain = c.InputDomain(port);
		const bool surface = domain && domain->Kind == SourceSocketKind::Surface;
		if (unit == 1 && !c.Input(port) && !surface) {
			Vector2 reference;
			if (!ResolveReferenceDimension(c, raw, reference)) return false;
			value.X *= reference.X;
			value.Y *= reference.Y;
		}
		return (std::isfinite(value.X) && std::isfinite(value.Y)) ||
			   c.Fail(
				   Status::UnsupportedExecution,
				   "Reference coordinates exceed the native arithmetic range",
				   port
			   );
	}
	inline bool StoreComplexPixel(
		NodeContext &c, Image &image, uint32_t x, uint32_t y, Rgba pixel, std::string_view output
	) {
		if (!WritePixel(image, x, y, pixel))
			return c.Fail(
				Status::UnsupportedExecution, "Generator sample is nonfinite or exceeds surface range", output
			);
		if (const auto *mask = c.Input("mask")) {
			pixel = SourceSafeDrawPixel(image, x, y);
			if (DescribeSurfaceFormat(image.Format)->Channels != 1) {
				const double u = (x + .5) / image.Width, v = (y + .5) / image.Height;
				const auto sample = SampleNearest(*mask, u, v);
				pixel[3] *= (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
			}
			for (auto &channel : pixel)
				channel = Quantize(channel) / 255.;
			if (!WritePixel(image, x, y, pixel))
				return c.Fail(
					Status::UnsupportedExecution, "Masked generator sample exceeds surface range", "mask"
				);
		}
		return true;
	}
}
