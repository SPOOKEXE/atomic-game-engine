#include "../PixelBuilderPayload.hpp"
#include "../SourceNineSlice.hpp"
#include "../TimelineDrivers.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	bool SurfaceSize(NodeContext &, double, double, uint32_t &, uint32_t &);
	bool SourceNineSlice(NodeContext &context) {
		if (const Value *input = context.Find("surface_in");
			input && (std::holds_alternative<AtlasValue>(*input) ||
					  std::holds_alternative<DynamicSurfaceValue>(*input))) {
			// The source tests surface_exists directly before initializing its recipe.
			return context.SetOutputDiagnostic(
					   "surface_out",
					   Status::UnsupportedExecution,
					   "Nine Slice non-raw surface input requires observed previous outputs"
				   ) &&
				   context.SetOutputDiagnostic(
					   "dyna_surf",
					   Status::UnsupportedExecution,
					   "Nine Slice non-raw surface input requires observed previous outputs"
				   );
		}

		const Image *source = context.Input("surface_in");
		if (!source || !source->Width || !source->Height)
			return context.Fail(
				Status::InvalidValue, "Nine Slice requires its original surface", "surface_in"
			);
		Vector2 dimension = context.Vec2("dimension", {1, 1});
		const int64_t dimensionUnit = context.Integer("dimension_unit", 1),
					  filling = context.Integer("filling_modes");
		if (dimensionUnit < 0 || dimensionUnit > 1 || filling < 0 || filling > 1)
			return context.Fail(Status::InvalidValue, "Nine Slice dimension unit or filling mode is invalid");
		if (!context.IsLinked("dimension") && dimensionUnit == 1) {
			dimension.X *= context.Project.SurfaceWidth;
			dimension.Y *= context.Project.SurfaceHeight;
		}
		Vector4 splice{};
		if (const Value *value = context.Find("splice")) {
			if (const auto *v = std::get_if<Vector4>(value))
				splice = *v;
			else if (const auto *v = std::get_if<Vector3>(value))
				splice = {v->X, v->Y, v->Z, 0};
			else if (const auto *v = std::get_if<Vector2>(value))
				splice = {v->X, v->Y, 0, 0};
			else if (std::holds_alternative<double>(*value) || std::holds_alternative<int64_t>(*value) ||
					 std::holds_alternative<bool>(*value) || std::holds_alternative<EnumValue>(*value)) {
				const double scalar = context.Scalar("splice");
				splice = {scalar, scalar, scalar, scalar};
			} else
				return context.Fail(
					Status::TypeMismatch, "Nine Slice splice requires resolved numeric values", "splice"
				);
		}
		const int64_t spliceUnit = context.Integer("splice_unit", 1);
		if (spliceUnit < 0 || spliceUnit > 1)
			return context.Fail(Status::InvalidValue, "Nine Slice splice unit is invalid", "splice_unit");
		if (spliceUnit == 1) {
			splice.X *= source->Width;
			splice.Z *= source->Width;
			splice.Y *= source->Height;
			splice.W *= source->Height;
		}
		for (double *component : {&splice.X, &splice.Y, &splice.Z, &splice.W}) {
			if (!std::isfinite(*component) || std::abs(*component) > Limits::MaximumDimension * 4)
				return context.Fail(
					Status::LimitExceeded, "Nine Slice splice exceeds bounded geometry", "splice"
				);
			*component = DriverRoundHalfEven(*component);
		}
		uint32_t width = 0, height = 0;
		if (!SurfaceSize(context, dimension.X, dimension.Y, width, height)) return false;
		const auto sampler = ReadSampler(context);
		if (!SupportedSampler(context, sampler)) return false;
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		const bool signedGeometry = splice.X < 0 || splice.Y < 0 || splice.Z < 0 || splice.W < 0 ||
									dimension.X < splice.X + splice.Z || dimension.Y < splice.Y + splice.W;
		const uint64_t pixels = uint64_t(width) * height,
					   rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		const uint64_t sampleCost = sampler.Interpolation == 4	 ? 144
									: sampler.Interpolation == 3 ? 64
									: sampler.Interpolation == 2 ? 4
																 : 1;
		if (!signedGeometry && pixels > 64000000 / (10 * sampleCost + 3) / rows)
			return context.Fail(
				Status::LimitExceeded, "Nine Slice whole processor batch exceeds work budget"
			);
		const uint64_t recipeBytes =
			sizeof(PixelBuilderData) + source->Pixels.size() + context.Authored.Id.size() + 1024;
		if (!context.ReserveOutput(recipeBytes, "dyna_surf")) return false;
		DynamicSurfaceValue dynamic;
		auto &data = dynamic.Data.emplace();
		data.OwnerNodeId = context.Authored.Id;
		data.MaximumImageDimension = context.Request.MaximumImageDimension;
		data.RequireSourceGpuRasterCoverage = context.Request.RequireSourceGpuRasterCoverage;
		data.BaseDimension = {double(source->Width), double(source->Height)};
		data.NineSlice.emplace(
			SourceNineSliceRecipe{*source, splice, filling, sampler.Interpolation, sampler.Oversample}
		);
		if (signedGeometry || context.Request.RequireSourceGpuRasterCoverage) {
			context.SetValue("dyna_surf", std::move(dynamic));
			return context.FailureCode == Status::Ok &&
				   context.SetOutputDiagnostic(
					   "surface_out",
					   Status::UnsupportedExecution,
					   "Nine Slice requested part coverage requires a licensed renderer observation"
				   );
		}
		auto scratch = context.ReserveWorkspace(pixels * 4, "surface_out");
		if (!scratch) return false;
		Image stage{width, height, std::vector<uint8_t>(pixels * 4), 0};
		if (!StageSourceNineSlice(context, *data.NineSlice, dimension, stage, {1, 1, 1, 1})) return false;
		Image *output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				auto value = TextureInterpolated(
					stage,
					(x + .5) / stage.Width,
					(y + .5) / stage.Height,
					sampler,
					{double(source->Width), double(source->Height)}
				);
				const double alpha = value[3];
				for (double &channel : value)
					channel *= alpha;
				if (!WritePixel(*output, x, y, value))
					return context.Fail(
						Status::InvalidValue, "Nine Slice Normal outer draw exceeds numeric format"
					);
			}
		context.SetValue("dyna_surf", std::move(dynamic));
		return context.FailureCode == Status::Ok;
	}
}
