#include "Source2DComplexGenerator.hpp"
#include "SourceInterpret.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t CRISTAL_BASE_WORK = 512, CRISTAL_STEP_WORK = 128;
		template <class V> std::optional<double> CristalNumber(const V &value) {
			if (const auto *number = std::get_if<double>(&value)) return *number;
			if (const auto *integer = std::get_if<int64_t>(&value)) return double(*integer);
			return std::nullopt;
		}
		bool CristalPair(NodeContext &c, const Value &value, std::string_view port, Vector2 &pair) {
			if (const auto *point = std::get_if<Vector2>(&value))
				pair = *point;
			else if (const auto number = CristalNumber(value))
				pair = {*number, *number};
			else if (const auto *array = std::get_if<ArrayValue>(&value);
					 array && array->Nested.empty() && array->Items.empty()) {
				pair = {};
				for (size_t i = 0; i < std::min<size_t>(2, array->Elements.size()); ++i) {
					const auto number = CristalNumber(array->Elements[i]);
					if (!number)
						return c.Fail(
							Status::UnsupportedExecution, "Cristal tuple needs numeric components", port
						);
					(i == 0 ? pair.X : pair.Y) = *number;
				}
			} else
				return c.Fail(Status::UnsupportedExecution, "Cristal tuple shape is not represented", port);
			return (std::isfinite(pair.X) && std::isfinite(pair.Y)) ||
				   c.Fail(Status::InvalidValue, "Cristal tuple must be finite", port);
		}
		bool CristalTuple(NodeContext &c, std::string_view port, Vector2 fallback, Vector2 &pair) {
			const auto *value = c.Find(port);
			if (!value) {
				pair = fallback;
				return true;
			}
			return CristalPair(c, *value, port, pair);
		}
		template <class ValueLike>
		bool CristalIterationLeaf(NodeContext &c, const ValueLike &value, uint64_t &maximum) {
			double count = 0;
			if (const auto *integer = std::get_if<int64_t>(&value))
				count = double(*integer);
			else if (const auto *number = std::get_if<double>(&value))
				count = source2d::GeneratorRoundHalfEven(*number);
			else
				return c.Fail(Status::InvalidValue, "Cristal Iteration requires numeric input", "iteration");
			if (!std::isfinite(count))
				return c.Fail(Status::InvalidValue, "Cristal Iteration must be finite", "iteration");
			if (count < -2147483648.)
				return c.Fail(
					Status::UnsupportedExecution,
					"Cristal iteration is outside the source signed-int uniform profile",
					"iteration"
				);
			if (count >
				double((source2d::COMPLEX_GENERATOR_WORK_LIMIT - CRISTAL_BASE_WORK) / CRISTAL_STEP_WORK))
				return c.Fail(
					Status::LimitExceeded,
					"Cristal whole-array work exceeds the native CPU limit",
					"iteration"
				);
			if (count > 0) maximum = std::max(maximum, uint64_t(count));
			return true;
		}
		bool CristalIterationItems(
			NodeContext &c, const std::vector<SourceArrayItem> &items, size_t depth, uint64_t &maximum
		) {
			if (depth > Limits::MaximumArrayDepth)
				return c.Fail(
					Status::LimitExceeded, "Cristal iteration nesting exceeds native limits", "iteration"
				);
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!CristalIterationLeaf(c, *leaf, maximum)) return false;
				} else if (!CristalIterationItems(
							   c, std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1, maximum
						   ))
					return false;
			}
			return true;
		}
		bool CristalWork(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			uint64_t iterations = 15;
			if (const auto *original = source2d::GeneratorOriginal(c, "iteration")) {
				iterations = 0;
				if (const auto *array = std::get_if<ArrayValue>(original)) {
					for (const auto &leaf : array->Elements)
						if (!CristalIterationLeaf(c, leaf, iterations)) return false;
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							if (!CristalIterationLeaf(c, leaf, iterations)) return false;
					if (!CristalIterationItems(c, array->Items, 0, iterations)) return false;
				} else if (!CristalIterationLeaf(c, *original, iterations))
					return false;
			}
			return source2d::ComplexBatchAdmission(
				c, CRISTAL_BASE_WORK + iterations * CRISTAL_STEP_WORK, "surface_out", "iteration"
			);
		}
		double CristalFract(double x) {
			return x - std::floor(x);
		}
		double CristalSmooth(double x) {
			const double t = std::clamp(x / .15, 0., 1.);
			return t * t * (3 - 2 * t);
		}
		Vector2 CristalRotate(Vector2 p, double c, double s) {
			return {p.X * c - p.Y * s, p.X * s + p.Y * c};
		}
		bool CristalOil(
			NodeContext &c,
			Vector2 pos,
			int64_t iterations,
			double seed,
			double phase,
			Rgba color,
			Rgba &result
		) {
			if (!std::isfinite(pos.X) || !std::isfinite(pos.Y))
				return c.Fail(
					Status::UnsupportedExecution,
					"Cristal coordinates exceed native arithmetic range",
					"scale"
				);
			Vector2 add{std::abs(CristalFract(pos.X) - .5) * .5, std::abs(CristalFract(pos.Y) - .5) * .5},
				q{};
			double weight = 0, frequency = 2.2;
			const double gain = 6.6 / double(iterations);
			const double sed = (seed - 100000 * std::floor(seed / 100000)) / 10;
			if (!std::isfinite(sed))
				return c.Fail(
					Status::UnsupportedExecution,
					"Cristal seed modulo exceeds native arithmetic range",
					"seed"
				);
			const double angle = phase * std::numbers::pi / 180, co = std::cos(angle), si = std::sin(angle);
			const double addAngle = 5 * std::numbers::pi / 180, addCo = std::cos(addAngle),
						 addSi = std::sin(addAngle);
			for (int64_t i = 0; i < iterations; ++i) {
				pos = CristalRotate(pos, co, si);
				// The earlier pos*s+sed assignment is overwritten before it is read.
				q = {std::cos(pos.X * frequency + add.X + sed), std::cos(pos.Y * frequency + add.Y + sed)};
				if (!std::isfinite(q.X) || !std::isfinite(q.Y))
					return c.Fail(
						Status::UnsupportedExecution,
						"Cristal recurrence exceeds native arithmetic range",
						"iteration"
					);
				weight += (std::sin((q.X * .3 + q.Y * .3) * 6.283184) * .25 + .25) * gain;
				frequency *= 1.07;
				add.X += std::cos(CristalSmooth(q.X));
				add.Y += std::cos(CristalSmooth(q.Y));
				add = CristalRotate(add, addCo, addSi);
				add.X *= 1.232;
				add.Y *= 1.232;
			}
			weight = std::pow(weight, 4.504);
			const double edge = std::abs(CristalFract(q.X * -.240) - .5);
			if (!(weight > 0) || !std::isfinite(weight) || !(edge > 0) || !std::isfinite(edge))
				return c.Fail(
					Status::UnsupportedExecution, "Cristal output normalization is undefined", "iteration"
				);
			for (size_t channel = 0; channel < 3; ++channel) {
				const double value = color[channel] / edge * .5 / weight;
				if (!std::isfinite(value))
					return c.Fail(
						Status::UnsupportedExecution,
						"Cristal Gamma color exceeds native arithmetic range",
						"gamma"
					);
				result[channel] = std::clamp(value, 0., 1.);
			}
			result[3] = 1;
			return true;
		}
	}
	bool SourceCristalNoise(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.cristal_noise");
		if (!CristalWork(c)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		source2d::ComplexCanvas canvas;
		if (!source2d::ResolveComplexCanvas(c, canvas)) return false;
		if (!c.Find("seed"))
			return c.Fail(Status::UnsupportedExecution, "Cristal requires an authored source seed", "seed");
		const double seed = c.Scalar("seed"), phase = c.Scalar("phase"), gamma = c.Scalar("gamma", 1);
		const int64_t iterations = c.Integer("iteration", 15);
		if (iterations <= 0)
			return c.Fail(
				Status::UnsupportedExecution,
				"Cristal nonpositive Iteration leaves a zero output divisor",
				"iteration"
			);
		Vector2 position, scale, levelIn, levelOut;
		if (!source2d::ReferenceVector(c, "position", canvas.Raw, position) ||
			!CristalTuple(c, "scale", {1, 1}, scale) || !CristalTuple(c, "level_in", {0, 1}, levelIn) ||
			!CristalTuple(c, "level_out", {0, 1}, levelOut))
			return false;
		if (levelIn.X == levelIn.Y)
			return c.Fail(Status::UnsupportedExecution, "Cristal Level In divides by zero", "level_in");
		Colour color{255, 255, 255, 255};
		if (const auto *value = c.Find("color")) {
			const auto converted =
				std::visit([](const auto &raw) { return InterpretPackedColour(raw); }, *value);
			if (!converted)
				return c.Fail(
					Status::UnsupportedExecution,
					"Cristal Color needs an integral packed color or typed color",
					"color"
				);
			color = *converted;
		}
		if (c.FailureCode != Status::Ok) return false;
		const Rgba rgb{color.Red / 255. * gamma, color.Green / 255. * gamma, color.Blue / 255. * gamma, 1};
		Image *output = c.NewImage("surface_out", canvas.Width, canvas.Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < canvas.Height; ++y)
			for (uint32_t x = 0; x < canvas.Width; ++x) {
				double alpha;
				const auto uv =
					source2d::GeneratorUv(c, (x + .5) / canvas.Width, (y + .5) / canvas.Height, alpha);
				const Vector2 pos{
					(uv.X - position.X / canvas.Raw.X) * scale.X,
					(uv.Y * canvas.Raw.Y / canvas.Raw.X - position.Y / canvas.Raw.Y) * scale.Y
				};
				Rgba value{};
				if (!CristalOil(c, pos, iterations, seed, phase, rgb, value)) return false;
				for (size_t channel = 0; channel < 3; ++channel)
					value[channel] = levelOut.X + (levelOut.Y - levelOut.X) * (value[channel] - levelIn.X) /
													  (levelIn.Y - levelIn.X);
				value[3] = alpha;
				if (!source2d::StoreComplexPixel(c, *output, x, y, value, "surface_out")) return false;
			}
		return c.FailureCode == Status::Ok;
	}
}
