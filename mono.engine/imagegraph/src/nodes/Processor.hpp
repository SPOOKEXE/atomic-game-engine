#pragma once

// Shared behavior of Pixel Composer's Node_Processor family, used by catalogue executors.
//
// A processor reads source input 0 as its main surface. When its Active input is false it copies that
// surface to output 0 unchanged. Otherwise output surfaces take input 0's size, or the project surface
// without one. Output storage follows the selected surface format. Normalized formats clamp and
// quantize samples; floating formats retain finite signed values. Mask, Mix and Channel apply afterward.

#include "../NodeExecutors.hpp"
#include "../PixelOps.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace engine::imagegraph::detail {
	using Rgba = std::array<double, 4>;

	inline uint8_t Quantize(double value) {
		if (!(value > 0.0)) return 0;
		if (value >= 1.0) return 255;
		return static_cast<uint8_t>(std::lround(value * 255.0));
	}

	inline Rgba ReadPixel(const Image &image, uint32_t x, uint32_t y) {
		Rgba pixel{};
		LoadSurfacePixel(image, x, y, pixel);
		return pixel;
	}

	inline bool WritePixel(Image &image, uint32_t x, uint32_t y, const Rgba &colour) {
		return StoreSurfacePixel(image, x, y, colour);
	}

	// Nearest texel for a normalized coordinate, matching an unfiltered texture2D read with clamp.
	inline Rgba SampleNearest(const Image &image, double u, double v) {
		const double fx = std::floor(u * image.Width);
		const double fy = std::floor(v * image.Height);
		const uint32_t x = static_cast<uint32_t>(std::clamp(fx, 0.0, static_cast<double>(image.Width - 1)));
		const uint32_t y = static_cast<uint32_t>(std::clamp(fy, 0.0, static_cast<double>(image.Height - 1)));
		return ReadPixel(image, x, y);
	}

	// Bilinear texture2D with clamp-to-edge addressing.
	inline Rgba BilinearClamp(const Image &image, double u, double v) {
		const double px = u * image.Width - 0.5, py = v * image.Height - 0.5;
		const double fx = std::floor(px), fy = std::floor(py);
		const double ax = px - fx, ay = py - fy;
		const auto texel = [&](double x, double y) {
			const auto cx = static_cast<uint32_t>(std::clamp(x, 0.0, static_cast<double>(image.Width - 1)));
			const auto cy = static_cast<uint32_t>(std::clamp(y, 0.0, static_cast<double>(image.Height - 1)));
			return ReadPixel(image, cx, cy);
		};
		const Rgba a = texel(fx, fy), b = texel(fx + 1, fy), c = texel(fx, fy + 1), d = texel(fx + 1, fy + 1);
		Rgba result{};
		for (size_t channel = 0; channel < 4; channel++) {
			const double top = a[channel] + (b[channel] - a[channel]) * ax;
			const double bottom = c[channel] + (d[channel] - c[channel]) * ax;
			result[channel] = top + (bottom - top) * ay;
		}
		return result;
	}

	inline std::optional<SurfaceFormat>
	ResolveProcessorSurfaceFormat(NodeContext &context, const Image *source) {
		const auto *attribute = FindCatalogueInput(context.Entry, "attribute_color_depth");
		if (!attribute) return SurfaceFormat::RGBA8Unorm;
		if (attribute->Default.empty() && !context.Find("attribute_color_depth")) {
			context.Fail(
				Status::UnsupportedExecution,
				"source surface format default is unresolved",
				"attribute_color_depth"
			);
			return std::nullopt;
		}
		const int64_t depth = context.Integer("attribute_color_depth", source ? 0 : 1);
		if (depth == 0) return source ? source->Format : SurfaceFormat::RGBA8Unorm;
		if (depth == 1) {
			if (context.InheritedSurfaceFormat) return context.InheritedSurfaceFormat;
			context.Fail(
				Status::UnsupportedExecution,
				"inherited Input surface format cannot be resolved",
				"attribute_color_depth"
			);
			return std::nullopt;
		}
		const auto format = SourceSurfaceFormat(depth);
		if (!format)
			context.Fail(Status::InvalidValue, "surface format choice is invalid", "attribute_color_depth");
		return format;
	}

	// The input whose Pixel Composer index is 0, when it is a surface.
	inline std::string_view MainSurfaceId(const NodeContext &context) {
		for (const CatalogueInput &input : context.Entry.Inputs)
			if (input.SourceIndex == 0 && input.Type == ValueType::Image) return input.Id;
		return {};
	}

	inline std::string_view FirstImageOutputId(const NodeContext &context) {
		for (const CatalogueOutput &output : context.Entry.Outputs)
			if (output.Type == ValueType::Image) return output.Id;
		return {};
	}

	// Handles the inactive copy. Returns true when the node is inactive and output 0 has been written.
	inline bool CopyWhenInactive(NodeContext &context, bool &failed) {
		failed = false;
		if (!context.Find("active") || context.Boolean("active", true)) return false;
		const Image *source = context.Input(MainSurfaceId(context));
		if (!source) {
			failed = !context.Fail(Status::InvalidValue, "image input is missing", MainSurfaceId(context));
			return true;
		}
		Image *out =
			context.NewImage(FirstImageOutputId(context), source->Width, source->Height, source->Format);
		if (!out) {
			failed = true;
			return true;
		}
		out->Pixels = source->Pixels;
		return true;
	}

	// Applies the processor Mask, Mix, Invert mask, Mask feather and Channel inputs, when declared.
	inline void FinishProcessor(NodeContext &context, const Image &original, Image &edited) {
		auto featherCharge = context.ReserveWorkspace(0, "mask_feather");
		if (!featherCharge) return;
		const Image *mask = context.Input("mask");
		Image feathered;
		const double feather = context.Scalar("mask_feather", 0.0);
		if (mask && feather > 0.0) {
			const double radius = std::max(1.0, std::round(feather));
			if (!std::isfinite(radius) || radius > std::numeric_limits<int>::max()) {
				context.Fail(
					Status::LimitExceeded, "mask feather scratch exceeds supported radius", "mask_feather"
				);
				return;
			}
			const uint64_t bytes = 2 * mask->Pixels.size() + uint64_t(radius) * sizeof(double);
			auto requiredCharge = context.ReserveWorkspace(bytes, "mask_feather");
			if (!requiredCharge) return;
			*featherCharge = std::move(*requiredCharge);
			feathered = FeatherMask(*mask, feather);
			if (!ValidSurfaceLayout(feathered, Limits::MaximumDimension, Limits::MaximumOutputBytes)) {
				context.Fail(
					Status::InvalidValue, "mask feather output exceeds numeric surface range", "mask_feather"
				);
				return;
			}
			mask = &feathered;
		}
		if (!ApplyMaskMix(
				original,
				edited,
				mask,
				context.Scalar("mix", 1.0),
				context.Boolean("invert_mask", false),
				context.Boolean("mask_alpha_only", false)
			))
			context.Fail(Status::InvalidValue, "processor mask mix exceeds numeric surface range", "mix");
		if (context.Find("channel") && !ApplyChannels(original, edited, context.Integer("channel", 15)))
			context.Fail(
				Status::InvalidValue, "processor channel output exceeds numeric surface range", "channel"
			);
	}

	// Runs a per-pixel fragment function over input 0 and applies the standard processor finish.
	// `shade(x, y, u, v)` returns the unclamped shader colour for one output pixel.
	template <class Shade> bool RunPixelProcessor(NodeContext &context, Shade &&shade) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const std::string_view sourceId = MainSurfaceId(context);
		const Image *source = context.Input(sourceId);
		if (!source) return context.Fail(Status::InvalidValue, "image input is missing", sourceId);
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		Image *out = context.NewImage(FirstImageOutputId(context), source->Width, source->Height, *format);
		if (!out) return false;
		for (uint32_t y = 0; y < out->Height; y++) {
			for (uint32_t x = 0; x < out->Width; x++) {
				const double u = (x + 0.5) / out->Width;
				const double v = (y + 0.5) / out->Height;
				if (!WritePixel(*out, x, y, shade(*source, x, y, u, v)))
					return context.Fail(
						Status::InvalidValue,
						"processor sample exceeds numeric surface range",
						FirstImageOutputId(context)
					);
			}
		}
		FinishProcessor(context, *source, *out);
		return context.FailureCode == Status::Ok;
	}

	// Dimension units affect authored values only. Native output limits replace source device limits.
	inline bool
	ResolveDimension(NodeContext &context, std::string_view id, uint32_t &width, uint32_t &height) {
		Vector2 size = context.Vec2(id, Vector2{1.0, 1.0});
		const int64_t unit = context.Integer(std::string(id) + "_unit", 0);
		if (unit < 0 || unit > 1) return context.Fail(Status::InvalidValue, "dimension unit is invalid", id);
		if (!context.IsLinked(id)) {
			if (unit == 1) {
				size.X *= context.Project.SurfaceWidth;
				size.Y *= context.Project.SurfaceHeight;
			}
		}
		if (!std::isfinite(size.X) || !std::isfinite(size.Y))
			return context.Fail(Status::InvalidValue, "dimensions must be finite", id);
		const auto roundEven = [](double value) {
			const double lower = std::floor(value), fraction = value - lower;
			return lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.0) != 0));
		};
		const double x = std::max(1.0, roundEven(size.X)), y = std::max(1.0, roundEven(size.Y));
		if (x > Limits::MaximumDimension || y > Limits::MaximumDimension)
			return context.Fail(Status::LimitExceeded, "dimensions exceed native limits", id);
		width = static_cast<uint32_t>(x);
		height = static_cast<uint32_t>(y);
		return true;
	}

	// A setUnitSimple vector in pixels: its "<id>_unit" Reference (1) scales by the reference surface size.
	inline Vector2 UnitVector(const NodeContext &context, std::string_view id, double width, double height) {
		Vector2 value = context.Vec2(id);
		if (context.Integer(std::string(id) + "_unit", 0) == 1) {
			value.X *= width;
			value.Y *= height;
		}
		return value;
	}

	// nodeValueUnit.convertUnit for a scalar: a Reference (1) value scales by the reference surface width.
	inline double UnitScale(const NodeContext &context, std::string_view id, double width) {
		return context.Integer(std::string(id) + "_unit", 0) == 1 ? width : 1.0;
	}

	// shader_set_f_map: while "<id>_mapped" is on and "<id>_map" is linked, the shader mixes the
	// "<id>_map_range" pair by the map texel's mean RGB at the output coordinate. Otherwise it uses the
	// value. With `filtered`, the node draws under gpu_set_tex_filter(true) and the map read is bilinear.
	inline double
	MappedScalar(const NodeContext &context, std::string_view id, double u, double v, bool filtered = false) {
		const std::string base(id);
		const Image *map = context.Input(base + "_map");
		if (!map || !context.Boolean(base + "_mapped", false)) return context.Scalar(id);
		const Vector2 range = context.Vec2(base + "_map_range");
		const Rgba texel = filtered ? BilinearClamp(*map, u, v) : SampleNearest(*map, u, v);
		const double amount = (texel[0] + texel[1] + texel[2]) / 3.0;
		return range.X + (range.Y - range.X) * amount;
	}
}
