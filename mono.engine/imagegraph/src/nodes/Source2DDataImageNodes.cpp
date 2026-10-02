#include "Gradient.hpp"
#include "Source2DGenerator.hpp"

namespace engine::imagegraph::detail {
	namespace {
		Rgba InterpretColour(NodeContext &context, double value) {
			const int64_t mode = context.Integer("mode");
			if (mode == 1) {
				const auto *palette = std::get_if<ArrayValue>(context.Find("palette"));
				if (!palette || palette->ElementType != ValueType::Colour || !palette->Nested.empty() ||
					palette->Elements.empty() || palette->Elements.size() > 256) {
					context.Fail(
						Status::InvalidValue, "Interpret palette requires 1 to 256 colours", "palette"
					);
					return {};
				}
				if (!std::isfinite(value) || std::trunc(value) < 0 ||
					value > double(std::numeric_limits<int32_t>::max())) {
					context.Fail(
						Status::InvalidValue, "Palette index must fit a nonnegative shader integer", "number"
					);
					return {};
				}
				const size_t index = size_t(value) % palette->Elements.size();
				const auto *colour = std::get_if<Colour>(&palette->Elements[index]);
				if (!colour) {
					context.Fail(Status::InvalidValue, "Interpret palette contains a non-colour", "palette");
					return {};
				}
				return {
					colour->Red / 255.0, colour->Green / 255.0, colour->Blue / 255.0, colour->Alpha / 255.0
				};
			}
			const Vector2 range = context.Vec2("range", {0, 1});
			const double grey = (value - range.X) / (range.Y - range.X);
			if (mode == 0) return {grey, grey, grey, 1};
			const auto *gradient = std::get_if<Gradient>(context.Find("gradient"));
			if (!gradient || !std::isfinite(grey)) {
				context.Fail(Status::InvalidValue, "Interpret gradient requires finite progress", "gradient");
				return {};
			}
			return GradientEval(
				ReadGradient(context, "gradient", *gradient),
				ShaderFract(ShaderFract(grey + context.Scalar("shift")) + 1)
			);
		}
	}

	bool InterpretMatrix(NodeContext &context) {
		const auto *matrix = std::get_if<MatrixValue>(context.Find("matrix"));
		if (!matrix || !matrix->Columns || !matrix->Rows ||
			uint64_t(matrix->Columns) * matrix->Rows != matrix->Values.size())
			return context.Fail(Status::InvalidValue, "A complete numeric matrix is required", "matrix");
		if (matrix->Values.size() > 256)
			return context.Fail(
				Status::LimitExceeded, "Interpret matrix exceeds 256 source shader values", "matrix"
			);
		Vector2 offset = context.Vec2("offset");
		const auto roundEven = [](double value) {
			const double lower = std::floor(value), fraction = value - lower;
			return lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2) != 0));
		};
		offset.X = roundEven(offset.X);
		offset.Y = roundEven(offset.Y);
		return source2d::RunGenerator(
			context, [&](uint32_t, uint32_t, uint32_t x, uint32_t y, double, double) {
				const auto wrap = [](double value, uint32_t dimension) {
					return value - std::floor(value / dimension) * dimension;
				};
				const double column = wrap(x - offset.X, matrix->Columns),
							 row = wrap(y - offset.Y, matrix->Rows);
				const size_t index = size_t(row * matrix->Columns + column);
				if (index >= matrix->Values.size()) {
					context.Fail(
						Status::InvalidValue, "Interpret matrix offset exceeds shader indices", "offset"
					);
					return Rgba{};
				}
				return InterpretColour(context, matrix->Values[index]);
			}
		);
	}
	bool MatrixColorApply(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *surface = context.Input("surface_in");
		if (!surface)
			return context.Fail(Status::InvalidValue, "Matrix Color Apply requires a surface", "surface_in");
		std::array<double, 9> matrix{};
		if (const Value *value = context.Find("matrix")) {
			const auto *payload = std::get_if<MatrixValue>(value);
			if (!payload)
				return context.Fail(
					Status::InvalidValue, "Matrix Color Apply requires a typed matrix", "matrix"
				);
			std::copy_n(
				payload->Values.begin(), std::min(payload->Values.size(), matrix.size()), matrix.begin()
			);
		}
		const double intensity = context.Scalar("intensity", 1);
		const auto format = ResolveProcessorSurfaceFormat(context, surface);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", surface->Width, surface->Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < surface->Height; ++y)
			for (uint32_t x = 0; x < surface->Width; ++x) {
				const auto original = ReadPixel(*surface, x, y);
				auto result = original;
				for (size_t channel = 0; channel < 3; ++channel) {
					double target = 0;
					for (size_t component = 0; component < 3; ++component)
						target += original[component] * matrix[channel * 3 + component];
					result[channel] += (target - result[channel]) * intensity;
				}
				if (!WritePixel(*output, x, y, result))
					return context.Fail(Status::InvalidValue, "Matrix Color Apply sample is nonfinite");
			}
		FinishProcessor(context, *surface, *output);
		return context.FailureCode == Status::Ok;
	}

}
