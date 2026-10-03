#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "Gradient.hpp"
#include "Source2DReferenceUnits.hpp"

#include <numbers>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t STRIPE_WORK_LIMIT = 64000000;
		template <class V, class F> void StripeLeaves(const V &value, F &visit);
		template <class F> void StripeItems(const std::vector<SourceArrayItem> &items, F &visit) {
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
					StripeLeaves(*leaf, visit);
				else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
					StripeItems(*children, visit);
			}
		}
		template <class V, class F> void StripeLeaves(const V &value, F &visit) {
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) {
					for (const auto &leaf : array->Elements)
						StripeLeaves(leaf, visit);
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							StripeLeaves(leaf, visit);
					StripeItems(array->Items, visit);
					return;
				}
			}
			visit(value);
		}
		bool StripeBatchAdmission(NodeContext &c, SurfaceFormat format) {
			if (c.ProcessorRow != 0) return true;
			if (!source2d::PreflightGeneratorDimensions(c)) return false;
			source2d::GeneratorDimensionBounds dimensions, units;
			if (!source2d::GeneratorDimensionBoundsFor(c, "dimension", dimensions) ||
				!source2d::GeneratorDimensionBoundsFor(c, "dimension_unit", units))
				return false;
			if (!dimensions.HasValue) dimensions.WidthMaximum = dimensions.HeightMaximum = 1;
			double width = dimensions.WidthMaximum, height = dimensions.HeightMaximum;
			if (c.IsLinked("dimension")) {
				if (const auto *image = c.Input("dimension")) {
					width = std::max(width, double(image->Width));
					height = std::max(height, double(image->Height));
				}
				for (const auto &[port, frames] : c.ImageArrays)
					if (port == "dimension" && frames)
						for (const auto &image : frames->Images) {
							width = std::max(width, double(image.Width));
							height = std::max(height, double(image.Height));
						}
			} else {
				const unsigned modes =
					units.Units ? units.Units : unsigned(1u << c.Integer("dimension_unit", 1));
				double xFactor = (modes & 1) ? 1 : 0, yFactor = xFactor;
				if (modes & 2) {
					xFactor = std::max(xFactor, double(c.Project.SurfaceWidth));
					yFactor = std::max(yFactor, double(c.Project.SurfaceHeight));
				}
				if (modes & 4) {
					if (const auto *mask = c.Input("mask")) {
						xFactor = std::max(xFactor, double(mask->Width));
						yFactor = std::max(yFactor, double(mask->Height));
					}
					for (const auto &[port, frames] : c.ImageArrays)
						if (port == "mask" && frames)
							for (const auto &image : frames->Images) {
								xFactor = std::max(xFactor, double(image.Width));
								yFactor = std::max(yFactor, double(image.Height));
							}
				}
				width *= xFactor;
				height *= yFactor;
			}
			const auto side = [](double n) {
				return uint64_t(std::max(1., source2d::GeneratorRoundHalfEven(n)));
			};
			const uint64_t pixels = side(width) * side(height), rows = std::max<size_t>(1, c.ProcessorCount);

			size_t keys = 0;
			auto gradient = [&](const auto &leaf) {
				if (const auto *g = std::get_if<Gradient>(&leaf)) keys = std::max(keys, g->Keys.size());
			};
			if (const auto *value = source2d::GeneratorOriginal(c, "colors")) StripeLeaves(*value, gradient);
			const uint64_t perPixel = 12 + keys;
			if (pixels > STRIPE_WORK_LIMIT / rows || pixels * rows > STRIPE_WORK_LIMIT / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Stripe whole-array work exceeds the native CPU limit", "dimension"
				);
			auto charge = c.ReserveWorkspace(
				pixels * rows * DescribeSurfaceFormat(format)->BytesPerPixel, "surface_out"
			);
			return bool(charge);
		}
		struct StripeControl {
			Vector2 Range;
			const Image *Map = nullptr;
		};
		bool
		ReadStripeControl(NodeContext &c, std::string_view port, double fallback, StripeControl &control) {
			if (SourceRangeMapped(c, port)) {
				if (!ReadSourceMappedRange(c, port, control.Range)) return false;
				control.Map = c.Input(std::string(port) + "_map");
			} else {
				const double value = c.Scalar(port, fallback);
				control.Range = {value, value};
			}
			return true;
		}
		double StripeSample(const StripeControl &control, double u, double v) {
			if (!control.Map) return control.Range.X;
			const auto sample = SampleNearest(*control.Map, u, v);
			return control.Range.X +
				   (control.Range.Y - control.Range.X) * (sample[0] + sample[1] + sample[2]) / 3;
		}
		double StripeMod(double a, double b) {
			return a - b * std::floor(a / b);
		}
		Rgba StripeColour(const Colour &p) {
			return {p.Red / 255., p.Green / 255., p.Blue / 255., p.Alpha / 255.};
		}
		Rgba StripeMix(const Rgba &a, const Rgba &b, double t) {
			Rgba result{};
			for (size_t channel = 0; channel < 4; ++channel)
				result[channel] = a[channel] + (b[channel] - a[channel]) * t;
			return result;
		}
	}
	bool SourceStripe(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.stripe");
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format || !StripeBatchAdmission(c, *format)) return false;
		const Image *mask = c.Input("mask");
		uint32_t width = 0, height = 0;
		if (!source2d::ResolveGeneratorDimensions(c, mask, width, height)) return false;
		auto dimension = c.Vec2("dimension", {1, 1});
		if (!c.IsLinked("dimension")) {
			const auto unit = c.Integer("dimension_unit", 1);
			if (unit == 1) {
				dimension.X *= c.Project.SurfaceWidth;
				dimension.Y *= c.Project.SurfaceHeight;
			}
			if (unit == 2) {
				dimension.X *= mask->Width;
				dimension.Y *= mask->Height;
			}
		}
		if (dimension.X == 0 || dimension.Y == 0)
			return c.Fail(Status::UnsupportedExecution, "Stripe raw canvas divides by zero", "dimension");
		const auto type = c.Integer("type"), coloring = c.Integer("coloring");
		if (type < 0 || type > 2 || coloring < 0 || coloring > 2)
			return c.Fail(Status::InvalidValue, "Stripe source choice is invalid");
		StripeControl size, angle, ratio, random;
		if (!ReadStripeControl(c, "size", .25, size) || !ReadStripeControl(c, "angle", 0, angle) ||
			!ReadStripeControl(c, "strip_ratio", .5, ratio) || !ReadStripeControl(c, "random", 0, random))
			return false;
		const auto sizeUnit = c.Integer("size_unit", 1), positionUnit = c.Integer("position_unit", 1);
		if (sizeUnit < 0 || sizeUnit > 1 || positionUnit < 0 || positionUnit > 1)
			return c.Fail(Status::InvalidValue, "Stripe Reference unit is invalid");
		const auto *original = source2d::GeneratorOriginal(c, "size");
		const bool originalScalar = original && (std::holds_alternative<double>(*original) ||
												 std::holds_alternative<int64_t>(*original));
		const bool syntheticPair = SourceRangeMapped(c, "size") && !c.IsLinked("size") &&
								   c.IsCatalogueDefault("size").value_or(false);
		// Units are resolved once before source processor rows; mapped Float-display pairs stay unscaled.
		Vector2 reference = dimension;
		if ((sizeUnit == 1 && originalScalar && !syntheticPair) ||
			(positionUnit == 1 && !c.Input("position")))
			if (!source2d::ResolveReferenceDimension(c, dimension, reference)) return false;
		if (sizeUnit == 1 && originalScalar && !syntheticPair) {
			size.Range.X *= reference.X;
			size.Range.Y *= reference.X;
		}
		auto position = c.Vec2("position", {.5, .5});
		if (positionUnit == 1 && !c.Input("position")) {
			position.X *= reference.X;
			position.Y *= reference.Y;
		}
		const double progress = c.Scalar("progress", .5), shift = c.Scalar("shift"), seed = c.Scalar("seed");
		if (!c.Find("seed") && (coloring == 2 || random.Range.X != 0 || random.Range.Y != 0))
			return c.Fail(
				Status::UnsupportedExecution,
				"Stripe stochastic coloring or boundaries require the authored source seed",
				"seed"
			);
		const Value *paletteValue = c.Find("colors_2"), *gradientValue = c.Find("colors");
		const auto *palette = paletteValue ? std::get_if<ArrayValue>(paletteValue) : nullptr;
		const auto *gradient = gradientValue ? std::get_if<Gradient>(gradientValue) : nullptr;
		if (coloring == 1 && (!palette || palette->Elements.empty() || palette->Elements.size() > 256 ||
							  !palette->Items.empty() || !palette->Nested.empty()))
			return c.Fail(
				Status::UnsupportedExecution, "Stripe GLSL palette requires 1 to 256 flat colors", "colors_2"
			);
		if (coloring == 1)
			for (const auto &value : palette->Elements)
				if (!std::holds_alternative<Colour>(value))
					return c.Fail(Status::TypeMismatch, "Stripe palette requires colors", "colors_2");
		if (coloring == 2 && !gradient)
			return c.Fail(Status::InvalidValue, "Stripe requires gradient controls", "colors");
		GradientSampler sampler;
		if (coloring == 2) {
			sampler = ReadGradient(c, "colors", *gradient);
			if (!sampler.Map && (gradient->Keys.empty() || gradient->Keys.size() > GRADIENT_KEY_SLOTS))
				return c.Fail(
					Status::UnsupportedExecution, "Stripe GLSL gradient requires 1 to 64 keys", "colors"
				);
		}
		const auto color0 = source2d::InputColour(c, "color_1", {255, 255, 255, 255}),
				   color1 = source2d::InputColour(c, "color_2", {0, 0, 0, 255});
		const auto hash = [&](double slot) {
			return ShaderFract(
				std::sin(slot * 12.9898 + slot * 78.233) * StripeMod(43758.5453123 + seed, 100000) / 10
			);
		};
		Image *output = c.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				const double amount = StripeSample(size, u, v),
							 ang = StripeSample(angle, u, v) * std::numbers::pi / 180;
				if (amount == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Stripe selected Size divides by zero", "size"
					);
				const double amo = dimension.X / amount, a = 1 / amo;
				double alpha = 1;
				const auto uv = source2d::GeneratorUv(c, u, v, alpha);
				const double prog =
					(uv.X - position.X / dimension.X) * (dimension.X / dimension.Y) * std::cos(ang) -
					(uv.Y - position.Y / dimension.Y) * std::sin(ang);
				const double slot = std::floor(prog / a), rnd = StripeSample(random, u, v),
							 rat = StripeSample(ratio, u, v) + .001;
				const double ground = (slot + (hash(slot) * 2 - 1) * rnd * .5) * a;
				const double ceiling = (slot + (hash(slot + 1) * 2 - 1) * rnd * .5 + 1) * a;
				const double phase = (prog - ground) / (ceiling - ground) + progress;
				if (!std::isfinite(phase) || ceiling == ground || !std::isfinite(slot))
					return c.Fail(
						Status::UnsupportedExecution, "Stripe source cell arithmetic is undefined", "size"
					);
				const double s = ShaderFract(phase);
				Rgba result{};
				if (coloring == 0) {
					if (type == 0)
						result = s >= rat ? color0 : color1;
					else {
						double t = std::sin(s * 2 * 3.14159265359) * .5 + .5;
						if (type == 2) {
							const double px = 3 / std::max(dimension.X, dimension.Y);
							if (!(px > 0) || !std::isfinite(px))
								return c.Fail(
									Status::UnsupportedExecution,
									"Stripe AA smoothstep has unordered edges",
									"dimension"
								);
							t = std::clamp((std::sin(s * 2 * 3.14159265359) + px) / (2 * px), 0., 1.);
							t = t * t * (3 - 2 * t);
						}
						result = StripeMix(color0, color1, t);
					}
				} else if (coloring == 1) {
					const double count = double(palette->Elements.size());
					const double index = StripeMod(s > rat ? slot : slot + 1, count);
					if (!std::isfinite(index) || index < 0 || index >= count)
						return c.Fail(
							Status::UnsupportedExecution, "Stripe palette index is undefined", "colors_2"
						);
					const size_t ind = size_t(index);
					result = StripeColour(std::get<Colour>(palette->Elements[ind]));
					if (type == 1) {
						const auto c0 = StripeColour(
							std::get<Colour>(palette->Elements
												 [s > rat ? ind
														  : (ind + palette->Elements.size() - 1) %
																palette->Elements.size()])
						);
						const auto c1 = StripeColour(
							std::get<Colour>(
								palette->Elements[s > rat ? (ind + 1) % palette->Elements.size() : ind]
							)
						);
						// Source intentionally does not normalize interpolation by each band's width.
						result = StripeMix(c0, c1, s > rat ? s - rat : s + (1 - rat));
					}
				} else
					result = GradientEval(
						sampler, ShaderFract(ShaderFract(hash(s > rat ? slot : slot + 1) + shift) + 1)
					);
				result[3] *= alpha;
				if (!WritePixel(*output, x, y, result))
					return c.Fail(
						Status::UnsupportedExecution,
						"Stripe shader sample is nonfinite or exceeds surface range",
						"surface_out"
					);
				if (mask) {
					// mask_apply_empty passes through RGBA8; safe single-red drawing overrides its shader.
					result = SourceSafeDrawPixel(*output, x, y);
					if (DescribeSurfaceFormat(*format)->Channels != 1) {
						const auto m = SampleNearest(*mask, u, v);
						result[3] *= (m[0] + m[1] + m[2]) / 3 * m[3];
					}
					for (auto &channel : result)
						channel = Quantize(channel) / 255.;
					if (!WritePixel(*output, x, y, result))
						return c.Fail(
							Status::InvalidValue, "Stripe masked sample exceeds surface range", "surface_out"
						);
				}
			}
		return c.FailureCode == Status::Ok;
	}
}
