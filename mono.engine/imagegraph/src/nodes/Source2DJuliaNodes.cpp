#include "Source2DComplexGenerator.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		template <class ValueLike>
		bool JuliaIterationLeaf(NodeContext &c, const ValueLike &value, uint64_t &maximum) {
			double count = 0;
			if (const auto *integer = std::get_if<int64_t>(&value))
				count = double(*integer);
			else if (const auto *number = std::get_if<double>(&value))
				count = source2d::GeneratorRoundHalfEven(*number);
			else
				return c.Fail(
					Status::InvalidValue, "Julia Max Iteration requires numeric input", "max_iteration"
				);
			if (!std::isfinite(count))
				return c.Fail(Status::InvalidValue, "Julia Max Iteration must be finite", "max_iteration");
			if (count < -2147483648.)
				return c.Fail(
					Status::UnsupportedExecution,
					"Julia iteration is outside the source signed-int uniform profile",
					"max_iteration"
				);
			if (count > double(
							(source2d::COMPLEX_GENERATOR_WORK_LIMIT - source2d::JULIA_PIXEL_BASE_WORK) /
							source2d::JULIA_ITERATION_WORK
						))
				return c.Fail(
					Status::LimitExceeded,
					"Julia whole-array work exceeds the native CPU limit",
					"max_iteration"
				);
			if (count > 0) maximum = std::max(maximum, uint64_t(count));
			return true;
		}
		bool JuliaIterationItems(
			NodeContext &c, const std::vector<SourceArrayItem> &items, size_t depth, uint64_t &maximum
		) {
			if (depth > Limits::MaximumArrayDepth)
				return c.Fail(
					Status::LimitExceeded, "Julia iteration nesting exceeds native limits", "max_iteration"
				);
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!JuliaIterationLeaf(c, *leaf, maximum)) return false;
				} else if (!JuliaIterationItems(
							   c, std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1, maximum
						   ))
					return false;
			}
			return true;
		}
		bool JuliaWork(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			uint64_t iterations = 128;
			if (const auto *original = source2d::GeneratorOriginal(c, "max_iteration")) {
				iterations = 0;
				if (const auto *array = std::get_if<ArrayValue>(original)) {
					for (const auto &leaf : array->Elements)
						if (!JuliaIterationLeaf(c, leaf, iterations)) return false;
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							if (!JuliaIterationLeaf(c, leaf, iterations)) return false;
					if (!JuliaIterationItems(c, array->Items, 0, iterations)) return false;
				} else if (!JuliaIterationLeaf(c, *original, iterations))
					return false;
			}
			return source2d::ComplexBatchAdmission(
				c,
				source2d::JULIA_PIXEL_BASE_WORK + iterations * source2d::JULIA_ITERATION_WORK,
				"surface",
				"max_iteration"
			);
		}
	}
	bool SourceJuliaSet(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.julia_set");
		if (!JuliaWork(c)) return false;
		source2d::ComplexCanvas canvas;
		if (!source2d::ResolveComplexCanvas(c, canvas)) return false;
		Vector2 constant, position;
		if (!source2d::ReferenceVector(c, "c", canvas.Raw, constant) ||
			!source2d::ReferenceVector(c, "position", canvas.Raw, position))
			return false;
		constant.X /= canvas.Raw.X;
		constant.Y /= canvas.Raw.Y;
		const auto scale = c.Vec2("scale", {1, 1});
		if (scale.X == 0 || scale.Y == 0)
			return c.Fail(Status::UnsupportedExecution, "Julia Scale divides by zero", "scale");
		const int64_t iterations = c.Integer("max_iteration", 128);
		if (iterations == 0)
			return c.Fail(
				Status::UnsupportedExecution, "Julia result divides by zero Max Iteration", "max_iteration"
			);
		const double threshold = c.Scalar("diverge_threshold", 4);
		const double radians = c.Scalar("rotation") * std::numbers::pi / 180;
		const double cosine = std::cos(radians), sine = std::sin(radians);
		// Node_Julia_Set has no color-depth attribute; Node.attrDepth returns RGBA8 even inside a group.
		Image *output = c.NewImage("surface", canvas.Width, canvas.Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		for (uint32_t y = 0; y < canvas.Height; ++y)
			for (uint32_t x = 0; x < canvas.Width; ++x) {
				const double u = (x + .5) / canvas.Width, v = (y + .5) / canvas.Height;
				double alpha = 1;
				const auto uv = source2d::GeneratorUv(c, u, v, alpha);
				const Vector2 p{
					(uv.X - position.X / canvas.Raw.X) * 4 / scale.X,
					(uv.Y - position.Y / canvas.Raw.Y) * 4 / scale.Y
				};
				Vector2 z{p.X * cosine - p.Y * sine, p.X * sine + p.Y * cosine};
				if (!std::isfinite(z.X) || !std::isfinite(z.Y))
					return c.Fail(
						Status::UnsupportedExecution,
						"Julia transformed coordinates exceed native arithmetic range",
						"scale"
					);
				int64_t escaped = iterations;
				for (int64_t n = 0; n < iterations; ++n) {
					const double distance = z.X * z.X + z.Y * z.Y;
					if (!std::isfinite(distance))
						return c.Fail(
							Status::UnsupportedExecution,
							"Julia recurrence exceeds native arithmetic range",
							"c"
						);
					if (distance > threshold) {
						escaped = n;
						break;
					}
					z = {z.X * z.X - z.Y * z.Y + constant.X, 2 * z.X * z.Y + constant.Y};
				}
				const double value = double(escaped) / double(iterations);
				if (!source2d::StoreComplexPixel(c, *output, x, y, {value, value, value, alpha}, "surface"))
					return false;
			}
		return c.FailureCode == Status::Ok;
	}
}
