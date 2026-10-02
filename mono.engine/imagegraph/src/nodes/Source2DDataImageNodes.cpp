#include "Source2DGenerator.hpp"
#include "SourceInterpret.hpp"

namespace engine::imagegraph::detail {

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
