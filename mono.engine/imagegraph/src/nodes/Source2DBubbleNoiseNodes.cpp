#include "Source2DComplexGenerator.hpp"

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t BUBBLE_BASE_WORK = 512, BUBBLE_STEP_WORK = 128;
		template <class V> std::optional<double> BubbleNumber(const V &value) {
			if (const auto *n = std::get_if<double>(&value)) return *n;
			if (const auto *n = std::get_if<int64_t>(&value)) return double(*n);
			return std::nullopt;
		}
		bool BubbleTuple(NodeContext &c, std::string_view port, Vector2 fallback, Vector2 &out) {
			const auto *value = c.Find(port);
			out = fallback;
			if (!value) return true;
			if (const auto *p = std::get_if<Vector2>(value))
				out = *p;
			else if (const auto n = BubbleNumber(*value))
				out = {*n, *n};
			else if (const auto *a = std::get_if<ArrayValue>(value);
					 a && a->Items.empty() && a->Nested.empty()) {
				out = {};
				for (size_t i = 0; i < std::min<size_t>(2, a->Elements.size()); ++i) {
					const auto n = BubbleNumber(a->Elements[i]);
					if (!n)
						return c.Fail(
							Status::UnsupportedExecution, "Bubble tuple needs numeric components", port
						);
					(i == 0 ? out.X : out.Y) = *n;
				}
			} else
				return c.Fail(Status::UnsupportedExecution, "Bubble tuple shape is not represented", port);
			return (std::isfinite(out.X) && std::isfinite(out.Y)) ||
				   c.Fail(Status::InvalidValue, "Bubble tuple must be finite", port);
		}
		bool BubbleCount(NodeContext &c, double product, uint64_t &count) {
			if (!std::isfinite(product) || product < double(std::numeric_limits<int32_t>::min()) ||
				product > double(std::numeric_limits<int32_t>::max()))
				return c.Fail(
					Status::UnsupportedExecution,
					"Bubble count is outside the shader signed-int range",
					"density"
				);
			count = product > 0 ? uint64_t(product) : 0;
			if (count > (source2d::COMPLEX_GENERATOR_WORK_LIMIT - BUBBLE_BASE_WORK) / BUBBLE_STEP_WORK)
				return c.Fail(
					Status::LimitExceeded, "Bubble whole-array work exceeds the native CPU limit", "density"
				);
			return true;
		}
		bool BubbleWork(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			if (!source2d::PreflightGeneratorDimensions(c)) return false;
			source2d::GeneratorDimensionBounds bounds, units;
			if (!source2d::GeneratorDimensionBoundsFor(c, "dimension", bounds) ||
				!source2d::GeneratorDimensionBoundsFor(c, "dimension_unit", units))
				return false;
			double width =
				bounds.HasValue ? std::max(std::abs(bounds.WidthMinimum), std::abs(bounds.WidthMaximum)) : 1;
			if (c.IsLinked("dimension")) {
				if (const auto *image = c.Input("dimension")) width = std::max(width, double(image->Width));
				for (const auto &[port, images] : c.ImageArrays)
					if (port == "dimension" && images)
						for (const auto &image : images->Images)
							width = std::max(width, double(image.Width));
			} else {
				const auto unit = c.Integer("dimension_unit", 1);
				if (unit < 0 || unit > 2)
					return c.Fail(Status::InvalidValue, "Dimension unit is invalid", "dimension_unit");
				const auto modes = units.Units ? units.Units : unsigned(1u << unit);
				double factor = (modes & 1) ? 1 : 0;
				if (modes & 2) factor = std::max(factor, double(c.Project.SurfaceWidth));
				if (modes & 4) {
					if (const auto *mask = c.Input("mask")) factor = std::max(factor, double(mask->Width));
					for (const auto &[port, images] : c.ImageArrays)
						if (port == "mask" && images)
							for (const auto &image : images->Images)
								factor = std::max(factor, double(image.Width));
				}
				width *= factor;
			}
			double density = .5;
			bool valid = true;
			const auto visit = [&](const auto &leaf) {
				const auto n = BubbleNumber(leaf);
				if (!n || !std::isfinite(*n)) {
					valid = false;
					return;
				}
				// Keep signed extremes too: shader int conversion must be representable even for no-loop
				// rows.
				uint64_t unused;
				if (!BubbleCount(c, *n * width, unused)) {
					valid = false;
					return;
				}
				density = std::max(density, std::abs(*n));
			};
			if (const auto *original = source2d::GeneratorOriginal(c, "density")) {
				density = 0;
				if (const auto *a = std::get_if<ArrayValue>(original)) {
					for (const auto &leaf : a->Elements)
						visit(leaf);
					for (const auto &row : a->Nested)
						for (const auto &leaf : row)
							visit(leaf);
					source2d::GeneratorVisitArrayItems(a->Items, 0, visit);
				} else
					visit(*original);
			}
			if (!valid)
				return c.FailureCode != Status::Ok ? false
												   : c.Fail(
														 Status::UnsupportedExecution,
														 "Bubble Density needs finite numeric leaves",
														 "density"
													 );
			uint64_t count;
			if (!BubbleCount(c, density * width, count)) return false;
			return source2d::ComplexBatchAdmission(
				c, BUBBLE_BASE_WORK + count * BUBBLE_STEP_WORK, "surface_out", "density"
			);
		}
		double BubbleRandom(double x, double y, double seed) {
			const double raw =
				std::sin((x + 1) * 2 + (y + 6) * 7) * (1 + (seed - 100000 * std::floor(seed / 100000)) / 10);
			return raw - std::floor(raw);
		}
		double BubbleSmooth(double lo, double hi, double value) {
			const double t = std::clamp((value - lo) / (hi - lo), 0., 1.);
			return t * t * (3 - 2 * t);
		}
	}
	bool SourceBubbleNoise(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.bubble_noise");
		if (!BubbleWork(c)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		source2d::ComplexCanvas canvas;
		if (!source2d::ResolveComplexCanvas(c, canvas)) return false;
		if (!c.Find("seed"))
			return c.Fail(Status::UnsupportedExecution, "Bubble requires an authored source seed", "seed");
		const double seed = c.Scalar("seed"), density = c.Scalar("density", .5),
					 thickness = c.Scalar("thickness");
		const auto mode = c.Integer("mode"), blend = c.Integer("blend_mode");
		Vector2 scale, opacity, levelIn, levelOut;
		if (!BubbleTuple(c, "scale", {.5, .8}, scale) || !BubbleTuple(c, "opacity", {0, 1}, opacity) ||
			!BubbleTuple(c, "level_in", {0, 1}, levelIn) || !BubbleTuple(c, "level_out", {0, 1}, levelOut))
			return false;
		if (c.FailureCode != Status::Ok) return false;
		if (mode < 0 || mode > 1) return c.Fail(Status::InvalidValue, "Bubble Mode is invalid", "mode");
		if (blend < 0 || blend > 1)
			return c.Fail(Status::InvalidValue, "Bubble Blend Mode is invalid", "blend_mode");
		if (levelIn.X == levelIn.Y)
			return c.Fail(Status::UnsupportedExecution, "Bubble Level In divides by zero", "level_in");
		uint64_t count;
		if (!BubbleCount(c, density * canvas.Raw.X, count)) return false;
		if (count && mode == 1 && !(thickness > 0))
			return c.Fail(
				Status::UnsupportedExecution, "Bubble Fill smoothstep edges are not increasing", "thickness"
			);
		const double lineWidth = std::max(std::min(1 / canvas.Raw.X, 1 / canvas.Raw.Y) / 2, thickness);
		if (count && mode == 0 && !(1 - lineWidth < 1))
			return c.Fail(
				Status::UnsupportedExecution, "Bubble Line smoothstep edges are not increasing", "thickness"
			);
		Image *output = c.NewImage("surface_out", canvas.Width, canvas.Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < canvas.Height; ++y)
			for (uint32_t x = 0; x < canvas.Width; ++x) {
				double alpha;
				const auto uv =
					source2d::GeneratorUv(c, (x + .5) / canvas.Width, (y + .5) / canvas.Height, alpha);
				const Vector2 pos{uv.X, uv.Y * canvas.Raw.Y / canvas.Raw.X};
				if (!std::isfinite(pos.X) || !std::isfinite(pos.Y))
					return c.Fail(
						Status::UnsupportedExecution,
						"Bubble coordinates exceed native arithmetic range",
						"dimension"
					);
				double weight = 0;
				for (uint64_t i = 0; i < count; ++i) {
					const double px = BubbleRandom(double(i), 1, seed), py = BubbleRandom(1, double(i), seed);
					if (!std::isfinite(px) || !std::isfinite(py))
						return c.Fail(
							Status::UnsupportedExecution,
							"Bubble seed hash exceeds native arithmetic range",
							"seed"
						);
					const double radius = scale.X + (scale.Y - scale.X) * BubbleRandom(2, double(i), seed);
					const double strength =
						opacity.X + (opacity.Y - opacity.X) * BubbleRandom(double(i), 2, seed);
					if (!std::isfinite(radius))
						return c.Fail(
							Status::UnsupportedExecution,
							"Bubble radius exceeds native arithmetic range",
							"scale"
						);
					if (!std::isfinite(strength))
						return c.Fail(
							Status::UnsupportedExecution,
							"Bubble opacity exceeds native arithmetic range",
							"opacity"
						);
					const double dx = pos.X - px, dy = pos.Y - py, dst = 1 - std::sqrt(dx * dx + dy * dy);
					if (!std::isfinite(dst))
						return c.Fail(
							Status::UnsupportedExecution,
							"Bubble distance exceeds native arithmetic range",
							"dimension"
						);
					if (mode == 1 && !(1 - radius - thickness < 1 - radius + thickness))
						return c.Fail(
							Status::UnsupportedExecution,
							"Bubble Fill edges collapse in native arithmetic",
							"thickness"
						);
					const double st =
						(mode == 0 ? BubbleSmooth(1 - lineWidth, 1, 1 - std::abs(dst - (1 - radius)))
								   : BubbleSmooth(
										 1 - radius - thickness, 1 - radius + thickness, std::max(0., dst)
									 )) *
						strength;
					if (!std::isfinite(st))
						return c.Fail(
							Status::UnsupportedExecution,
							"Bubble sample exceeds native arithmetic range",
							"scale"
						);
					weight = blend == 0 ? std::max(weight, st) : weight + st;
				}
				weight =
					levelOut.X + (levelOut.Y - levelOut.X) * (weight - levelIn.X) / (levelIn.Y - levelIn.X);
				if (!source2d::StoreComplexPixel(
						c, *output, x, y, {weight, weight, weight, alpha}, "surface_out"
					))
					return false;
			}
		return c.FailureCode == Status::Ok;
	}
}
