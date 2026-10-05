#pragma once
#include "Source2DMath.hpp"

#include <limits>

namespace engine::imagegraph::detail::source2d {
	struct GeneratorDimensionBounds {
		double WidthMinimum = std::numeric_limits<double>::infinity();
		double HeightMinimum = std::numeric_limits<double>::infinity();
		double WidthMaximum = -std::numeric_limits<double>::infinity();
		double HeightMaximum = -std::numeric_limits<double>::infinity();
		uint8_t Units = 0;
		bool HasValue = false;
	};

	inline const Value *GeneratorOriginal(const NodeContext &context, std::string_view port) {
		for (auto input = context.ProcessorOriginalValues.rbegin();
			 input != context.ProcessorOriginalValues.rend();
			 ++input)
			if (input->first == port) return input->second;
		for (const auto &[id, value] : context.Values)
			if (id == port) return &value;
		return context.Find(port);
	}

	inline bool GeneratorDimensionLeaf(
		NodeContext &context,
		std::string_view port,
		const ElementValue &leaf,
		GeneratorDimensionBounds &bounds
	) {
		const auto number = [&](double value) {
			if (!std::isfinite(value))
				return context.Fail(Status::InvalidValue, "dimensions must be finite", port);
			return true;
		};
		const auto include = [&](double width, double height) {
			bounds.WidthMinimum = std::min(bounds.WidthMinimum, width);
			bounds.HeightMinimum = std::min(bounds.HeightMinimum, height);
			bounds.WidthMaximum = std::max(bounds.WidthMaximum, width);
			bounds.HeightMaximum = std::max(bounds.HeightMaximum, height);
			bounds.HasValue = true;
		};
		if (port == "dimension") {
			if (const auto *value = std::get_if<Vector2>(&leaf)) {
				if (!number(value->X) || !number(value->Y)) return false;
				include(value->X, value->Y);
			} else if (const auto *value = std::get_if<double>(&leaf)) {
				if (!number(*value)) return false;
				include(*value, *value);
			} else if (const auto *value = std::get_if<int64_t>(&leaf)) {
				if (!number(double(*value))) return false;
				include(double(*value), double(*value));
			}
			return true;
		}
		int64_t unit = -1;
		if (const auto *value = std::get_if<EnumValue>(&leaf))
			unit = value->Value;
		else if (const auto *value = std::get_if<int64_t>(&leaf))
			unit = *value;
		else if (const auto *value = std::get_if<double>(&leaf);
				 value && std::isfinite(*value) && *value >= 0 && *value <= 2 && std::trunc(*value) == *value)
			unit = int64_t(*value);
		if (unit < 0 || unit > 2)
			return context.Fail(Status::InvalidValue, "dimension unit is invalid", "dimension");
		bounds.Units |= uint8_t(1u << unit);
		return true;
	}

	template <class VisitLeaf>
	void
	GeneratorVisitArrayItems(const std::vector<SourceArrayItem> &items, size_t depth, VisitLeaf &&visitLeaf) {
		if (depth > Limits::MaximumArrayDepth) return;
		for (const auto &item : items) {
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
				visitLeaf(*leaf);
			else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
				GeneratorVisitArrayItems(*children, depth + 1, visitLeaf);
		}
	}

	inline bool GeneratorDimensionBoundsFor(
		NodeContext &context, std::string_view port, GeneratorDimensionBounds &bounds
	) {
		const Value *value = GeneratorOriginal(context, port);
		if (!value) return true;
		const auto visit = [&](const ElementValue &leaf) {
			return GeneratorDimensionLeaf(context, port, leaf, bounds);
		};
		if (const auto *array = std::get_if<ArrayValue>(value)) {
			for (const auto &leaf : array->Elements)
				if (!visit(leaf)) return false;
			for (const auto &row : array->Nested)
				for (const auto &leaf : row)
					if (!visit(leaf)) return false;
			GeneratorVisitArrayItems(array->Items, 0, [&](const ElementValue &leaf) { visit(leaf); });
			return context.FailureCode == Status::Ok;
		}
		if (const auto *vector = std::get_if<Vector2>(value)) return visit(ElementValue{*vector});
		if (const auto *number = std::get_if<double>(value)) return visit(ElementValue{*number});
		if (const auto *integer = std::get_if<int64_t>(value)) return visit(ElementValue{*integer});
		if (const auto *unit = std::get_if<EnumValue>(value)) return visit(ElementValue{*unit});
		return true;
	}

	inline double GeneratorRoundHalfEven(double value) {
		const double lower = std::floor(value), fraction = value - lower;
		return lower + (fraction > 0.5 || (fraction == 0.5 && std::fmod(lower, 2.0) != 0));
	}

	inline bool GeneratorObserveDimensions(NodeContext &context, double width, double height) {
		if (!std::isfinite(width) || !std::isfinite(height))
			return context.Fail(Status::InvalidValue, "dimensions must be finite", "dimension");
		const double roundedWidth = std::max(1.0, GeneratorRoundHalfEven(width));
		const double roundedHeight = std::max(1.0, GeneratorRoundHalfEven(height));
		return (roundedWidth <= Limits::MaximumDimension && roundedHeight <= Limits::MaximumDimension) ||
			   context.Fail(Status::LimitExceeded, "dimensions exceed native limits", "dimension");
	}

	inline bool PreflightGeneratorDimensions(NodeContext &context) {
		if (context.ProcessorRow != 0) return true;
		GeneratorDimensionBounds size, units;
		if (!GeneratorDimensionBoundsFor(context, "dimension", size) ||
			!GeneratorDimensionBoundsFor(context, "dimension_unit", units))
			return false;
		if (context.IsLinked("dimension")) {
			const auto includeImage = [&](const Image &image) {
				size.WidthMinimum = std::min(size.WidthMinimum, double(image.Width));
				size.HeightMinimum = std::min(size.HeightMinimum, double(image.Height));
				size.WidthMaximum = std::max(size.WidthMaximum, double(image.Width));
				size.HeightMaximum = std::max(size.HeightMaximum, double(image.Height));
				size.HasValue = true;
			};
			if (const Image *image = context.Input("dimension")) includeImage(*image);
			for (const auto &[port, images] : context.ImageArrays)
				if (port == "dimension" && images)
					for (const Image &image : images->Images)
						includeImage(image);
		}
		if (!size.HasValue) {
			const Vector2 fallback = context.Vec2("dimension", {1, 1});
			size.WidthMinimum = size.WidthMaximum = fallback.X;
			size.HeightMinimum = size.HeightMaximum = fallback.Y;
			size.HasValue = true;
		}
		if (!units.Units) {
			const int64_t unit = context.Integer("dimension_unit", 1);
			if (unit < 0 || unit > 2)
				return context.Fail(Status::InvalidValue, "dimension unit is invalid", "dimension");
			units.Units = uint8_t(1u << unit);
		}
		const double width = size.WidthMaximum;
		const double height = size.HeightMaximum;
		if (context.IsLinked("dimension")) return GeneratorObserveDimensions(context, width, height);
		const bool pixel = (units.Units & 1u) != 0;
		const bool project = (units.Units & 2u) != 0;
		const bool maskUnit = (units.Units & 4u) != 0;
		if (pixel && !GeneratorObserveDimensions(context, width, height)) return false;
		if (project &&
			(!GeneratorObserveDimensions(
				 context, width * context.Project.SurfaceWidth, height * context.Project.SurfaceHeight
			 ) ||
			 !GeneratorObserveDimensions(
				 context,
				 size.WidthMinimum * context.Project.SurfaceWidth,
				 size.HeightMinimum * context.Project.SurfaceHeight
			 )))
			return false;
		if (maskUnit) {
			double maskWidth = 0, maskHeight = 0;
			if (const Image *mask = context.Input("mask")) {
				maskWidth = mask->Width;
				maskHeight = mask->Height;
			}
			for (const auto &[port, images] : context.ImageArrays)
				if (port == "mask" && images)
					for (const Image &image : images->Images) {
						maskWidth = std::max(maskWidth, double(image.Width));
						maskHeight = std::max(maskHeight, double(image.Height));
					}
			if (!maskWidth || !maskHeight)
				return context.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
			// Independent extrema conservatively bound every selected Dimension and Mask pairing.
			if (!GeneratorObserveDimensions(context, width * maskWidth, height * maskHeight) ||
				!GeneratorObserveDimensions(
					context, size.WidthMinimum * maskWidth, size.HeightMinimum * maskHeight
				))
				return false;
		}
		return true;
	}

	inline bool SourceMaskDimensionGenerator(std::string_view type) {
		return type == "pc.checker" || type == "pc.quasicrystal" || type == "pc.wave_interfere" ||
			   type == "pc.zigzag" || type == "pc.box_pattern" || type == "pc.fold_noise" ||
			   type == "pc.noise_aniso" || type == "pc.kisrhombille";
	}

	inline bool
	ResolveGeneratorDimensions(NodeContext &context, const Image *mask, uint32_t &width, uint32_t &height) {
		const int64_t unit = context.Integer("dimension_unit", 1);
		if (unit < 0 || unit > 2)
			return context.Fail(Status::InvalidValue, "dimension unit is invalid", "dimension");
		Vector2 size = context.Vec2("dimension", {1, 1});
		if (!context.IsLinked("dimension")) {
			if (unit == 1) {
				size.X *= context.Project.SurfaceWidth;
				size.Y *= context.Project.SurfaceHeight;
			} else if (unit == 2) {
				if (!mask)
					return context.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
				size.X *= mask->Width;
				size.Y *= mask->Height;
			}
		}
		if (!std::isfinite(size.X) || !std::isfinite(size.Y))
			return context.Fail(Status::InvalidValue, "dimensions must be finite", "dimension");
		const double roundedWidth = std::max(1.0, GeneratorRoundHalfEven(size.X));
		const double roundedHeight = std::max(1.0, GeneratorRoundHalfEven(size.Y));
		if (roundedWidth > Limits::MaximumDimension || roundedHeight > Limits::MaximumDimension)
			return context.Fail(Status::LimitExceeded, "dimensions exceed native limits", "dimension");
		width = uint32_t(roundedWidth);
		height = uint32_t(roundedHeight);
		return true;
	}
	inline Vector2
	GeneratorUv(const NodeContext &context, double u, double v, double &alpha, bool filtered = false) {
		alpha = 1;
		const Image *map = context.Input("uv_map");
		if (!map) return {u, v};
		const Rgba pixel = filtered ? BilinearClamp(*map, u, v) : SampleNearest(*map, u, v);
		const double amount = context.Scalar("uv_mix", 1);
		alpha = pixel[3];
		return {u + (pixel[0] - u) * amount, v + (1.0 - pixel[1] - v) * amount};
	}

	template <class Shade> bool RunGenerator(NodeContext &context, Shade &&shade) {
		uint32_t width = 0, height = 0;
		const Image *mask = context.Input("mask");
		if (SourceMaskDimensionGenerator(context.Entry.Type)) {
			if (!PreflightGeneratorDimensions(context) ||
				!ResolveGeneratorDimensions(context, mask, width, height))
				return false;
		} else if (context.Integer("dimension_unit", 1) == 2) {
			if (!mask) return context.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
			width = mask->Width;
			height = mask->Height;
		} else if (!ResolveDimension(context, "dimension", width, height))
			return false;
		const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + 0.5) / width, v = (y + 0.5) / height;
				const Rgba generated = shade(width, height, x, y, u, v);
				if (context.FailureCode != Status::Ok) return false;
				if (!WritePixel(*output, x, y, generated))
					return context.Fail(
						Status::InvalidValue, "Generator sample exceeds surface range", "surface_out"
					);
				if (mask) {
					Rgba colour = ReadPixel(*output, x, y);
					const Rgba sample = SampleNearest(*mask, u, v);
					colour[3] *= (sample[0] + sample[1] + sample[2]) / 3.0 * sample[3];
					for (double &channel : colour)
						channel = Quantize(channel) / 255.0;
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "Masked generator sample exceeds surface range", "mask"
						);
				}
			}
		return context.FailureCode == Status::Ok;
	}

	inline Rgba InputColour(const NodeContext &context, std::string_view port, Colour fallback = {}) {
		const Colour colour = context.Get<Colour>(port, fallback);
		return {colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0};
	}
}
