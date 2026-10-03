#pragma once
#include "../SourceSafeDraw.hpp"
#include "Source2DReferenceUnits.hpp"

#include <type_traits>
namespace engine::imagegraph::detail::source_pattern {
	constexpr uint64_t PATTERN_WORK_LIMIT = 64000000;
	template <class V, class F> void PatternLeaves(const V &value, F &visit);
	template <class F> void PatternItems(const std::vector<SourceArrayItem> &items, F &visit) {
		for (const auto &item : items) {
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
				PatternLeaves(*leaf, visit);
			else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
				PatternItems(*children, visit);
		}
	}
	template <class V, class F> void PatternLeaves(const V &value, F &visit) {
		if constexpr (std::is_same_v<V, Value>) {
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				for (const auto &leaf : array->Elements)
					PatternLeaves(leaf, visit);
				for (const auto &row : array->Nested)
					for (const auto &leaf : row)
						PatternLeaves(leaf, visit);
				PatternItems(array->Items, visit);
				return;
			}
		}
		visit(value);
	}
	inline bool PatternBatchAdmission(NodeContext &c, uint64_t perPixel) {
		if (c.ProcessorRow != 0) return true;
		if (!source2d::PreflightGeneratorDimensions(c)) return false;
		source2d::GeneratorDimensionBounds dimensions, units;
		if (!source2d::GeneratorDimensionBoundsFor(c, "dimension", dimensions) ||
			!source2d::GeneratorDimensionBoundsFor(c, "dimension_unit", units))
			return false;
		if (!dimensions.HasValue) dimensions.WidthMaximum = dimensions.HeightMaximum = 1;
		double width = dimensions.WidthMaximum, height = dimensions.HeightMaximum;
		if (c.IsLinked("dimension")) {
			if (const auto *image = c.Input("dimension")) {
				width = std::max(width, double(image->Width));
				height = std::max(height, double(image->Height));
			}
			for (const auto &[port, frames] : c.ImageArrays)
				if (port == "dimension" && frames)
					for (const auto &image : frames->Images) {
						width = std::max(width, double(image.Width));
						height = std::max(height, double(image.Height));
					}
		} else {
			const unsigned modes = units.Units ? units.Units : unsigned(1u << c.Integer("dimension_unit", 1));
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
				for (const auto &[port, frames] : c.ImageArrays)
					if (port == "mask" && frames)
						for (const auto &image : frames->Images) {
							xFactor = std::max(xFactor, double(image.Width));
							yFactor = std::max(yFactor, double(image.Height));
						}
			}
			width *= xFactor;
			height *= yFactor;
		}
		const auto side = [](double n) {
			return uint64_t(std::max(1., source2d::GeneratorRoundHalfEven(n)));
		};
		const uint64_t pixels = side(width) * side(height), rows = std::max<size_t>(1, c.ProcessorCount);

		if (pixels > PATTERN_WORK_LIMIT / rows || pixels * rows > PATTERN_WORK_LIMIT / perPixel)
			return c.Fail(
				Status::LimitExceeded, "Pattern whole-array work exceeds the native CPU limit", "dimension"
			);
		const uint64_t rowMetadata =
			2 * (sizeof(std::pair<std::string, Image>) + sizeof(ImageArrayItem) + std::string{}.capacity());
		auto charge = c.ReserveWorkspace(pixels * rows * 16 + rows * rowMetadata, "surface_out");
		return bool(charge);
	}

	inline bool Mask(NodeContext &c, Image &output, uint32_t x, uint32_t y, double u, double v) {
		const auto *mask = c.Input("mask");
		if (!mask) return true;
		auto colour = SourceSafeDrawPixel(output, x, y);
		if (DescribeSurfaceFormat(output.Format)->Channels != 1) {
			auto m = SampleNearest(*mask, u, v);
			colour[3] *= (m[0] + m[1] + m[2]) / 3 * m[3];
		}
		for (auto &channel : colour)
			channel = Quantize(channel) / 255.;
		return WritePixel(output, x, y, colour) ||
			   c.Fail(Status::UnsupportedExecution, "pattern masked pixel is undefined", "mask");
	}
	inline bool Dimensions(NodeContext &c, uint32_t &w, uint32_t &h, Vector2 &raw) {
		if (!source2d::ResolveGeneratorDimensions(c, c.Input("mask"), w, h)) return false;
		raw = c.Vec2("dimension", {1, 1});
		if (!c.IsLinked("dimension")) {
			auto unit = c.Integer("dimension_unit", 1);
			if (unit == 1) {
				raw.X *= c.Project.SurfaceWidth;
				raw.Y *= c.Project.SurfaceHeight;
			}
			if (unit == 2) {
				raw.X *= c.Input("mask")->Width;
				raw.Y *= c.Input("mask")->Height;
			}
		}
		return (raw.X != 0 && raw.Y != 0) ||
			   c.Fail(Status::UnsupportedExecution, "pattern raw dimension divides by zero", "dimension");
	}
	inline bool SurfaceGetter(const NodeContext &c, std::string_view port) {
		const auto domain = c.InputDomain(port);
		return c.IsLinked(port) && domain && domain->Kind == SourceSocketKind::Surface;
	}
	inline bool Position(NodeContext &c, Vector2 dimension, Vector2 fallback, Vector2 &position) {
		position = c.Vec2("position", fallback);
		auto unit = c.Integer("position_unit", 1);
		if (unit < 0 || unit > 1)
			return c.Fail(Status::InvalidValue, "pattern position unit is invalid", "position_unit");
		if (unit == 1 && !SurfaceGetter(c, "position")) {
			Vector2 reference;
			if (!source2d::ResolveReferenceDimension(c, dimension, reference)) return false;
			position.X *= reference.X;
			position.Y *= reference.Y;
		}
		return true;
	}
}
