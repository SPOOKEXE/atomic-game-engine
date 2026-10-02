#include "../SourceSafeDraw.hpp"
#include "../TimelineDrivers.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		bool WholeWork(NodeContext &context, uint64_t pixels, uint64_t perPixel, std::string_view port) {
			const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
			if (perPixel > 64000000 / rows || pixels > 64000000 / (rows * perPixel))
				return context.Fail(
					Status::LimitExceeded,
					"content operation complete processor batch exceeds work budget",
					port
				);
			return true;
		}
		bool ContentSingleChannel(const Image &image) {
			return image.Format == SurfaceFormat::R8Unorm || image.Format == SurfaceFormat::R16Float ||
				   image.Format == SurfaceFormat::R32Float;
		}
	} // namespace
	bool SourceGapContract(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Gap Contract requires its surface", "surface_in");
		const int64_t count = context.Integer("max_width", 8);
		if (count < -64000000 || count > 64000000)
			return context.Fail(
				Status::LimitExceeded, "Gap Contract iteration count exceeds bounded work", "max_width"
			);
		const uint64_t iterations = uint64_t(count < 0 ? -count : count),
					   pixels = uint64_t(source->Width) * source->Height;
		if (!WholeWork(context, pixels, 16 + iterations * 16, "max_width")) return false;
		uint64_t totalWork = pixels * (16 + iterations * 16);
		const Image *mask = context.Input("mask");
		const double feather = context.Scalar("mask_feather");
		if (mask && feather > 0) {
			if (!std::isfinite(feather) || feather > 64)
				return context.Fail(
					Status::LimitExceeded, "Gap Contract mask feather exceeds bounded work", "mask_feather"
				);
			const uint64_t maskCost = 16 + uint64_t(std::ceil(feather)) * 16;
			const uint64_t maskPixels = uint64_t(mask->Width) * mask->Height;
			if (!WholeWork(context, maskPixels, maskCost, "mask_feather")) return false;
			totalWork += maskPixels * maskCost;
		}
		if (!WholeWork(context, 1, totalWork, "max_width")) return false;
		auto scratch = context.ReserveWorkspace(pixels * 8, "surface_in");
		if (!scratch) return false;
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		Image buffers[2] = {
			{source->Width, source->Height, std::vector<uint8_t>(pixels * 4), 0},
			{source->Width, source->Height, std::vector<uint8_t>(pixels * 4), 0}
		};
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				Rgba p = SourceSafeDrawPixel(*source, x, y);
				// __channel_pre replaces sh_invert when the input has a single red
				// channel.
				if (context.Boolean("invert") && !ContentSingleChannel(*source))
					for (size_t c = 0; c < 3; ++c)
						p[c] = 1 - p[c];
				if (!WritePixel(buffers[1], x, y, p))
					return context.Fail(
						Status::InvalidValue, "Gap Contract staging exceeds numeric format", "surface_in"
					);
			}
		const int64_t oversample = ReadSampler(context).Oversample;
		size_t next = 0;
		for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
			const Image &input = buffers[1 - next];
			Image &output = buffers[next];
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x) {
					const auto binary = [&](int dx, int dy) {
						const auto p = SampleTextureSimple(
							input,
							(x + .5 + dx) / source->Width,
							(y + .5 + dy) / source->Height,
							oversample,
							false
						);
						return (p[0] + p[1] + p[2]) / 3 * p[3] >= .5;
					};
					const bool center = binary(0, 0);
					const std::array neighbors{
						binary(0, -1),
						binary(1, -1),
						binary(1, 0),
						binary(1, 1),
						binary(0, 1),
						binary(-1, 1),
						binary(-1, 0),
						binary(-1, -1)
					};
					unsigned transitions = 0, population = 0;
					for (size_t i = 0; i < 8; ++i) {
						population += neighbors[i];
						transitions += !neighbors[i] && neighbors[(i + 1) % 8];
					}
					const bool p2 = neighbors[0], p4 = neighbors[2], p6 = neighbors[4], p8 = neighbors[6];
					bool value = center;
					if (population >= 2 && population <= 6 && transitions == 1) {
						if (count >= 0 && center) {
							if ((next == 0 && (!p2 || !p4 || !p6) && (!p4 || !p6 || !p8)) ||
								(next == 1 && (!p2 || !p4 || !p8) && (!p2 || !p6 || !p8)))
								value = false;
						} else if (count < 0 && !center) {
							if ((next == 0 && (p2 || p4 || p6) && (p4 || p6 || p8)) ||
								(next == 1 && (p2 || p4 || p8) && (p2 || p6 || p8)))
								value = true;
						}
					}
					if (!WritePixel(output, x, y, {double(value), double(value), double(value), 1}))
						return context.Fail(Status::InvalidValue, "Gap Contract pass exceeds numeric format");
				}
			next = 1 - next;
		}
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				auto p = ReadPixel(buffers[1 - next], x, y);
				if (context.Boolean("keep_alpha")) p[3] = ReadPixel(*source, x, y)[3];
				if (!WritePixel(*output, x, y, p))
					return context.Fail(Status::InvalidValue, "Gap Contract output exceeds numeric format");
			}
		FinishProcessor(context, *source, *output);
		return context.FailureCode == Status::Ok;
	}
	bool SourceAlignContent(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Align Content requires its surface", "surface_in");
		const uint64_t pixels = uint64_t(source->Width) * source->Height;
		if (!WholeWork(context, pixels, 24, "surface_in")) return false;
		auto scratch = context.ReserveWorkspace(pixels * 4, "surface_in");
		if (!scratch) return false;
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		Colour bg{0, 0, 0, 0};
		if (const Value *value = context.Find("background")) {
			const auto *colour = std::get_if<Colour>(value);
			if (!colour)
				return context.Fail(
					Status::TypeMismatch, "Align Content background requires a resolved colour", "background"
				);
			bg = *colour;
		}
		const Rgba target{bg.Red / 255., bg.Green / 255., bg.Blue / 255., bg.Alpha / 255.};
		Image temporary{source->Width, source->Height, std::vector<uint8_t>(pixels * 4), 0};
		uint32_t minx = source->Width, miny = source->Height, maxx = 0, maxy = 0;
		bool content = false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				Rgba p = SourceSafeDrawPixel(*source, x, y);
				if (!ContentSingleChannel(*source) && p == target) p = {};
				if (!WritePixel(temporary, x, y, p))
					return context.Fail(Status::InvalidValue, "Align Content staging exceeds numeric format");
				// The pinned extension reads RGBA8 alpha bytes, rather than unquantized
				// source alpha.
				if (temporary.Pixels[(size_t(y) * source->Width + x) * 4 + 3]) {
					content = true;
					minx = std::min(minx, x);
					miny = std::min(miny, y);
					maxx = std::max(maxx, x);
					maxy = std::max(maxy, y);
				}
			}
		const double boundWidth = content ? maxx - minx + 1 : 0, boundHeight = content ? maxy - miny + 1 : 0;
		if (!content) minx = miny = 0;
		const auto anchor = context.Vec2("align_anchor", {.5, .5});
		Vector4 padding{};
		if (const Value *value = context.Find("pad_content")) {
			// The Padding getter repeats scalar values and zero-pads short source
			// tuples.
			if (const auto *vector = std::get_if<Vector4>(value))
				padding = *vector;
			else if (const auto *vector = std::get_if<Vector3>(value))
				padding = {vector->X, vector->Y, vector->Z, 0};
			else if (const auto *vector = std::get_if<Vector2>(value))
				padding = {vector->X, vector->Y, 0, 0};
			else if (std::holds_alternative<double>(*value) || std::holds_alternative<int64_t>(*value) ||
					 std::holds_alternative<bool>(*value) || std::holds_alternative<EnumValue>(*value)) {
				const double scalar = context.Scalar("pad_content");
				padding = {scalar, scalar, scalar, scalar};
			} else
				return context.Fail(
					Status::TypeMismatch,
					"Align Content padding requires resolved numeric values",
					"pad_content"
				);
		}
		const int64_t unit = context.Integer("pad_content_unit", 0);
		if (unit < 0 || unit > 1)
			return context.Fail(
				Status::InvalidValue, "Align Content padding unit is invalid", "pad_content_unit"
			);
		// IPadding applies alternating width/height units before its integer getter
		// rounds.
		if (unit == 1) {
			padding.X *= source->Width;
			padding.Z *= source->Width;
			padding.Y *= source->Height;
			padding.W *= source->Height;
		}
		for (double *p : {&padding.X, &padding.Y, &padding.Z, &padding.W}) {
			if (!std::isfinite(*p) || std::abs(*p) > Limits::MaximumDimension * 4)
				return context.Fail(
					Status::LimitExceeded,
					"Align Content padding exceeds native geometric bounds",
					"pad_content"
				);
			*p = DriverRoundHalfEven(*p);
		}
		const double sx =
			-double(minx) + anchor.X * (source->Width - boundWidth - padding.X - padding.Z) + padding.Z;
		const double sy =
			-double(miny) + anchor.Y * (source->Height - boundHeight - padding.W - padding.Y) + padding.Y;
		if (!std::isfinite(sx) || !std::isfinite(sy))
			return context.Fail(
				Status::InvalidValue, "Align Content translation must be finite", "align_anchor"
			);
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		const double dx = std::trunc(sx), dy = std::trunc(sy);
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				Rgba p = target;
				const double fx = x + .5 - dx, fy = y + .5 - dy;
				if (fx >= 0 && fy >= 0 && fx < source->Width && fy < source->Height) {
					const auto sample = Texture(temporary, fx / source->Width, fy / source->Height, false);
					for (size_t c = 0; c < 3; ++c)
						p[c] = sample[c] + target[c] * (1 - sample[3]);
					p[3] = sample[3] + target[3];
				}
				if (!WritePixel(*output, x, y, p))
					return context.Fail(Status::InvalidValue, "Align Content output exceeds numeric format");
			}
		return true;
	}
} // namespace engine::imagegraph::detail
