#include "../PixelOpsCurveColor.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	bool BlendEdge(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Blend Edge requires a surface", "surface_in");
		const int64_t mode = context.Integer("types");
		if (mode < 0 || mode > 2)
			return context.Fail(Status::InvalidValue, "Blend Edge type is invalid", "types");
		const bool curved = context.Boolean("smoothness_curved");
		const Value *curveValue = context.Find("smoothness_curve");
		const Curve *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (curved && (!curve || curve->Anchors.size() > 9 || !ValidColorCurve(*curve)))
			return context.Fail(
				Status::InvalidValue,
				"Blend Edge curve exceeds its source shader contract",
				"smoothness_curve"
			);
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		auto scratchCharge =
			context.ReserveWorkspace(mode == 0 ? uint64_t(source->Width) * source->Height * 4 : 0);
		if (!scratchCharge) return false;
		Image scratch;
		if (mode == 0)
			scratch = {
				source->Width,
				source->Height,
				std::vector<uint8_t>(uint64_t(source->Width) * source->Height * 4),
				0
			};
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		const auto pass = [&](const Image &input, Image &target, int axis) {
			for (uint32_t y = 0; y < input.Height; ++y)
				for (uint32_t x = 0; x < input.Width; ++x) {
					const double u = (x + .5) / input.Width, v = (y + .5) / input.Height;
					const double width = MappedScalar(context, "width", u, v),
								 blend = MappedScalar(context, "blending", u, v),
								 smooth = MappedScalar(context, "smoothness", u, v);
					if (width == 0 || blend == 0)
						return context.Fail(
							Status::UnsupportedExecution,
							"Blend Edge source division is undefined for zero width or blending"
						);
					const double coordinate = axis == 0 ? u : v;
					const double linear = std::clamp(
						1 - std::max(0.0, (1 - std::abs(coordinate - .5) * 2) / width - (1 - blend)) / blend,
						0.0,
						1.0
					);
					double eased = linear * linear * (3 - 2 * linear);
					if (curved && SampleColorCurveUnchecked(*curve, linear, eased) != CurveColorStatus::Ok)
						return context.Fail(
							Status::UnsupportedExecution,
							"Blend Edge source curve division is undefined",
							"smoothness_curve"
						);
					const double amount = linear + (eased - linear) * smooth;
					const Rgba first = ReadPixel(input, x, y),
							   second = SampleNearest(
								   input,
								   axis == 0 ? u + .5 - std::floor(u + .5) : u,
								   axis == 1 ? v + .5 - std::floor(v + .5) : v
							   );
					Rgba colour{};
					for (size_t channel = 0; channel < 4; ++channel)
						colour[channel] = first[channel] + (second[channel] - first[channel]) * amount;
					if (!WritePixel(target, x, y, colour))
						return context.Fail(Status::InvalidValue, "Blend Edge sample is nonfinite");
				}
			return true;
		};
		if (mode == 0) {
			if (!pass(*source, scratch, 0) || !pass(scratch, *output, 1)) return false;
		} else if (!pass(*source, *output, int(mode - 1)))
			return false;
		FinishProcessor(context, *source, *output);
		return context.FailureCode == Status::Ok;
	}
}
