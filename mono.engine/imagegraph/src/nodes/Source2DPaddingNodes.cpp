#include "../PixelBuilderPayload.hpp"
#include "../SourceSafeDraw.hpp"
#include "../TimelineDrivers.hpp"
#include "Processor.hpp"

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	bool SurfaceSize(NodeContext &, double, double, uint32_t &, uint32_t &);

	bool SourcePadding(NodeContext &context) {
		if (context.Find("active") && !context.Boolean("active", true)) {
			const Value *input = context.Find("surface_in");
			if (const auto *dynamic = input ? std::get_if<DynamicSurfaceValue>(input) : nullptr) {
				if (!ValidPixelBuilderPayload(*dynamic))
					return context.Fail(
						Status::InvalidValue,
						"Padding bypass requires a valid owned dynamic surface",
						"surface_in"
					);
				if (!context.ReserveOutput(RetainedPayloadBytes(*dynamic) + 32, "surface_out")) return false;
				context.SetValue("surface_out", *dynamic);
				return context.FailureCode == Status::Ok;
			}
		}
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		Image rendered;
		std::optional<AllocationReservation> renderedCharge;
		bool nineSlice = false;
		if (!source) {
			const Value *input = context.Find("surface_in");
			const auto *dynamic = input ? std::get_if<DynamicSurfaceValue>(input) : nullptr;
			if (dynamic && dynamic->Data) {
				const uint64_t budget = context.AvailableBytes();
				renderedCharge = context.ReserveWorkspace(budget, "surface_in");
				if (!renderedCharge) return false;
				Diagnostic diagnostic;
				const auto status = RasterizePixelBuilder(
					*dynamic,
					dynamic->Data->BaseDimension,
					rendered,
					diagnostic,
					budget,
					context.Request.RigidProvider
				);
				if (status != Status::Ok) return context.Fail(diagnostic, "surface_in");
				if (!renderedCharge->Resize(rendered.Pixels.capacity()))
					return context.Fail(
						Status::LimitExceeded, "Padding dynamic render exceeds byte budget", "surface_in"
					);
				source = &rendered;
				nineSlice = dynamic->Data->NineSlice.has_value();
			}
		}

		if (!source || !source->Width || !source->Height)
			return context.Fail(Status::InvalidValue, "Padding requires a nonempty surface", "surface_in");
		const int64_t mode = context.Integer("pad_mode"), fill = context.Integer("fill_method");
		const int64_t horizontal = context.Integer("h_align"), vertical = context.Integer("v_align");
		if (mode < 0 || mode > 1 || fill < 0 || fill > 2 || horizontal < 0 || horizontal > 2 ||
			vertical < 0 || vertical > 2)
			return context.Fail(Status::InvalidValue, "Padding mode or alignment is invalid");
		double requestedWidth = source->Width, requestedHeight = source->Height, left = 0, top = 0;
		if (mode == 0) {
			Vector4 padding{};
			if (const Value *value = context.Find("padding")) {
				if (const auto *v = std::get_if<Vector4>(value))
					padding = *v;
				else if (const auto *v = std::get_if<Vector3>(value))
					padding = {v->X, v->Y, v->Z, 0};
				else if (const auto *v = std::get_if<Vector2>(value))
					padding = {v->X, v->Y, 0, 0};
				else if (std::holds_alternative<double>(*value) || std::holds_alternative<int64_t>(*value) ||
						 std::holds_alternative<bool>(*value) || std::holds_alternative<EnumValue>(*value)) {
					const double scalar = context.Scalar("padding");
					padding = {scalar, scalar, scalar, scalar};
				} else
					return context.Fail(
						Status::TypeMismatch, "Padding requires resolved numeric values", "padding"
					);
			}
			const int64_t unit = context.Integer("padding_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(Status::InvalidValue, "Padding unit is invalid", "padding_unit");
			if (unit == 1) {
				padding.X *= source->Width;
				padding.Z *= source->Width;
				padding.Y *= source->Height;
				padding.W *= source->Height;
			}
			for (double *value : {&padding.X, &padding.Y, &padding.Z, &padding.W}) {
				if (!std::isfinite(*value) || std::abs(*value) > Limits::MaximumDimension * 4)
					return context.Fail(Status::LimitExceeded, "Padding exceeds bounded geometry", "padding");
				*value = DriverRoundHalfEven(*value);
			}
			requestedWidth += padding.X + padding.Z;
			requestedHeight += padding.Y + padding.W;
			left = padding.Z;
			top = padding.Y;
			// The source leaves its previous cached output untouched for these sizes.
			if (requestedWidth <= 1 || requestedHeight <= 1)
				return context.Fail(
					Status::UnsupportedExecution,
					"Padding requires an observed previous output when either Pad out dimension is at most "
					"one",
					"padding"
				);
		} else {
			auto dimension = context.Vec2("dimension", {32, 32});
			const int64_t unit = context.Integer("dimension_unit");
			if (unit < 0 || unit > 1)
				return context.Fail(
					Status::InvalidValue, "Padding dimension unit is invalid", "dimension_unit"
				);
			// Vec2 applies its unit to linked numeric values as well as authored values.
			if (unit == 1 && !context.Input("dimension")) {
				dimension.X *= source->Width;
				dimension.Y *= source->Height;
			}
			requestedWidth = dimension.X;
			requestedHeight = dimension.Y;
			left = horizontal == 0 ? 0 : (requestedWidth - source->Width) / (horizontal == 1 ? 2 : 1);
			top = vertical == 0 ? 0 : (requestedHeight - source->Height) / (vertical == 1 ? 2 : 1);
		}
		uint32_t width = 0, height = 0;
		if (!SurfaceSize(context, requestedWidth, requestedHeight, width, height)) return false;
		const uint64_t pixels = uint64_t(width) * height,
					   rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		const uint64_t passes = fill == 2 ? (std::max(width, height) + 15) / 16 + 2 : 1;
		if (passes > 64000000 / 16 / rows || pixels > 64000000 / (16 * passes * rows))
			return context.Fail(
				Status::LimitExceeded, "Padding complete processor batch exceeds work budget"
			);
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.SetOutputDiagnostic(
				"surface_out",
				Status::UnsupportedExecution,
				"Padding exact GPU draw coverage requires a licensed renderer observation"
			);
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		const Colour background = context.Get<Colour>("fill_color", {0, 0, 0, 255});
		const Rgba clear =
			fill == 1
				? Rgba{background.Red / 255., background.Green / 255., background.Blue / 255., background.Alpha / 255.}
				: Rgba{};
		left = std::trunc(left);
		top = std::trunc(top);
		bool opaque = true;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				Rgba value = clear;
				const double sx = x - left, sy = y - top;
				if (sx >= 0 && sy >= 0 && sx < source->Width && sy < source->Height) {
					value = SourceSafeDrawPixel(*source, uint32_t(sx), uint32_t(sy));
					if (fill == 1 || nineSlice)
						for (size_t channel = 0; channel < 4; ++channel)
							value[channel] = value[channel] * value[3] + clear[channel] * (1 - value[3]);
				}
				if (!WritePixel(*output, x, y, value))
					return context.Fail(Status::InvalidValue, "Padding output exceeds numeric format");
				opaque = opaque && std::lround(std::clamp(ReadPixel(*output, x, y)[3], 0., 1.) * 255) == 255;
			}
		if (fill == 2) {
			if (!opaque)
				return context.Fail(
					Status::UnsupportedExecution,
					"Padding Pixel Expand needs the unobserved sh_atlas resolution uniform for nonopaque "
					"pixels",
					"fill_method"
				);
			// Opaque pixels return before sh_atlas reads resolution. Its RGBA8 staging
			// still quantizes floating input even though the search is skipped.
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					auto value = ReadPixel(*output, x, y);
					for (double &channel : value)
						channel = std::lround(std::clamp(channel, 0., 1.) * 255) / 255.;
					if (!WritePixel(*output, x, y, value))
						return context.Fail(Status::InvalidValue, "Padding staging exceeds numeric format");
				}
		}
		return true;
	}
}
