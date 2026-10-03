#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "Gradient.hpp"
#include "Source2DReferenceUnits.hpp"

#include <numbers>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t DOTTED_WORK_LIMIT = 64000000;
		template <class V, class F> void DottedLeaves(const V &value, F &visit);
		template <class F> void DottedItems(const std::vector<SourceArrayItem> &items, F &visit) {
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
					DottedLeaves(*leaf, visit);
				else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
					DottedItems(*children, visit);
			}
		}
		template <class V, class F> void DottedLeaves(const V &value, F &visit) {
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) {
					for (const auto &leaf : array->Elements)
						DottedLeaves(leaf, visit);
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							DottedLeaves(leaf, visit);
					DottedItems(array->Items, visit);
					return;
				}
			}
			visit(value);
		}
		template <class V> std::optional<double> DottedNumber(const V &value) {
			if (const auto *number = std::get_if<double>(&value)) return *number;
			if (const auto *integer = std::get_if<int64_t>(&value)) return double(*integer);
			if (const auto *choice = std::get_if<EnumValue>(&value)) return double(choice->Value);
			return std::nullopt;
		}
		struct DottedExtrema {
			double Minimum = std::numeric_limits<double>::infinity(),
				   Maximum = -std::numeric_limits<double>::infinity();
		};
		DottedExtrema DottedNumbers(const NodeContext &c, std::string_view port, double fallback) {
			DottedExtrema range;
			auto visit = [&](const auto &leaf) {
				if (const auto number = DottedNumber(leaf)) {
					range.Minimum = std::min(range.Minimum, *number);
					range.Maximum = std::max(range.Maximum, *number);
				} else if (const auto *v = std::get_if<Vector2>(&leaf)) {
					range.Minimum = std::min({range.Minimum, v->X, v->Y});
					range.Maximum = std::max({range.Maximum, v->X, v->Y});
				}
			};
			if (const auto *v = source2d::GeneratorOriginal(c, port)) DottedLeaves(*v, visit);
			if (range.Minimum == std::numeric_limits<double>::infinity()) range = {fallback, fallback};
			return range;
		}
		bool DottedBatchAdmission(NodeContext &c, SurfaceFormat format) {
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
			const auto spacing = DottedNumbers(c, "spacing", 1), size = DottedNumbers(c, "size", .25),
					   sizeRange = DottedNumbers(c, "size_map_range", 0);
			const auto mapped = DottedNumbers(c, "size_mapped", 0),
					   sizeUnit = DottedNumbers(c, "size_unit", 1);
			double minimum = size.Minimum;
			bool anyMapped = mapped.Maximum != 0;
			// Boolean controls are separate variant alternatives.
			auto mappedLeaf = [&](const auto &leaf) {
				if (const auto *v = std::get_if<bool>(&leaf)) anyMapped |= *v;
			};
			if (const auto *v = source2d::GeneratorOriginal(c, "size_mapped")) DottedLeaves(*v, mappedLeaf);
			if (anyMapped && c.IsCatalogueDefault("size").value_or(false))
				minimum = std::min(minimum, sizeRange.Minimum);
			const Value *original = source2d::GeneratorOriginal(c, "size");
			const bool scalar = original && (std::holds_alternative<double>(*original) ||
											 std::holds_alternative<int64_t>(*original));
			const bool referenceScalar = scalar && sizeUnit.Maximum >= 1 &&
										 (!anyMapped || !c.IsCatalogueDefault("size").value_or(false));
			const bool pixelSize = !referenceScalar || sizeUnit.Minimum <= 0;
			double extentX = 16, extentY = 16;
			if (minimum > 0 && spacing.Minimum > 0) {
				const double divisor = minimum * spacing.Minimum * spacing.Minimum;
				double ratioX = pixelSize ? width : 0, ratioY = pixelSize ? height : 0;
				if (referenceScalar) {
					Vector2 reference;
					if (!source2d::ResolveFirstReferenceDimension(c, reference)) return false;
					// Every row uses the first prepared reference, not its own canvas width.
					ratioX = reference.X > 0 ? std::max(ratioX, width / reference.X)
											 : std::numeric_limits<double>::infinity();
					ratioY = reference.X > 0 ? std::max(ratioY, height / reference.X)
											 : std::numeric_limits<double>::infinity();
				}

				extentX = std::clamp(std::ceil(ratioX / divisor), 0., 16.);
				extentY = std::clamp(std::ceil(ratioY / divisor), 0., 16.);
			}
			size_t gradientKeys = 0;
			auto gradient = [&](const auto &leaf) {
				if (const auto *g = std::get_if<Gradient>(&leaf))
					gradientKeys = std::max(gradientKeys, g->Keys.size());
			};
			if (const auto *v = source2d::GeneratorOriginal(c, "gradient")) DottedLeaves(*v, gradient);
			const auto colourModes = DottedNumbers(c, "dot_color_mode", 0);
			const uint64_t perNeighbor =
				colourModes.Minimum <= 2 && colourModes.Maximum >= 2 ? 1 + gradientKeys : 1;
			const uint64_t taps = uint64_t(2 * extentX + 1) * uint64_t(2 * extentY + 1);
			const uint64_t perPixel = taps * perNeighbor + 3;
			if (pixels > DOTTED_WORK_LIMIT / rows || pixels * rows > DOTTED_WORK_LIMIT / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Dotted whole-array work exceeds the native CPU limit", "dimension"
				);
			// Color Depth is a static/inherited attribute, not a processor input row.
			const uint64_t bytesPerPixel = DescribeSurfaceFormat(format)->BytesPerPixel;
			// This temporary admission checks all output rows before NewImage publishes row zero.
			auto charge = c.ReserveWorkspace(pixels * rows * bytesPerPixel, "surface_out");
			return bool(charge);
		}
		struct DottedMapped {
			Vector2 Range;
			const Image *Map = nullptr;
		};
		bool DottedControl(NodeContext &c, std::string_view port, DottedMapped &control) {
			if (SourceRangeMapped(c, port)) {
				if (!ReadSourceMappedRange(c, port, control.Range)) return false;
				control.Map = c.Input(std::string(port) + "_map");
			} else {
				const double value = c.Scalar(port, port == "size" ? .25 : (port == "dot_size" ? .5 : 0));
				control.Range = {value, value};
			}
			return true;
		}
		double DottedSample(const DottedMapped &control, double u, double v) {
			if (!control.Map) return control.Range.X;
			const auto p = SampleNearest(*control.Map, u, v);
			return control.Range.X + (control.Range.Y - control.Range.X) * (p[0] + p[1] + p[2]) / 3;
		}
		double DottedMod(double a, double b) {
			return a - b * std::floor(a / b);
		}
		bool DottedFinite(NodeContext &c, Vector2 value, std::string_view port) {
			return (std::isfinite(value.X) && std::isfinite(value.Y)) ||
				   c.Fail(
					   Status::UnsupportedExecution, "Dotted source geometry has nonfinite arithmetic", port
				   );
		}
	}
	bool SourceDotted(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.dotted");
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format || !DottedBatchAdmission(c, *format)) return false;
		uint32_t width = 0, height = 0;
		const Image *mask = c.Input("mask");
		if (!source2d::ResolveGeneratorDimensions(c, mask, width, height)) return false;
		Vector2 dimension = c.Vec2("dimension", {1, 1});
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
			return c.Fail(Status::UnsupportedExecution, "Dotted raw canvas divides by zero", "dimension");
		const auto pattern = c.Integer("pattern"), render = c.Integer("render_mode"),
				   blend = c.Integer("blend_mode"), colourMode = c.Integer("dot_color_mode");
		if (pattern < 0 || pattern > 1 || render < 0 || render > 2 || blend < 0 || blend > 1 ||
			colourMode < 0 || colourMode > 3)
			return c.Fail(Status::InvalidValue, "Dotted source choice is invalid");
		DottedMapped size, angle, dot;
		if (!DottedControl(c, "size", size) || !DottedControl(c, "angle", angle) ||
			!DottedControl(c, "dot_size", dot))
			return false;
		const auto unit = c.Integer("size_unit", 1), positionUnit = c.Integer("position_unit", 1);
		if (unit < 0 || unit > 1 || positionUnit < 0 || positionUnit > 1)
			return c.Fail(Status::InvalidValue, "Dotted Reference unit is invalid");
		const Value *originalSize = source2d::GeneratorOriginal(c, "size");
		const bool originalScalar = originalSize && (std::holds_alternative<double>(*originalSize) ||
													 std::holds_alternative<int64_t>(*originalSize));
		// Float arrays do not enter nodeValueUnit's vector-display conversion branch.
		const bool syntheticPair = SourceRangeMapped(c, "size") && !c.IsLinked("size") &&
								   c.IsCatalogueDefault("size").value_or(false);
		Vector2 reference = dimension;
		if ((unit == 1 && originalScalar && !syntheticPair) || (positionUnit == 1 && !c.Input("position")))
			if (!source2d::ResolveReferenceDimension(c, dimension, reference)) return false;
		if (unit == 1 && originalScalar && !syntheticPair) {
			size.Range.X *= reference.X;
			size.Range.Y *= reference.X;
		}
		auto position = c.Vec2("position");
		if (positionUnit == 1 && !c.Input("position")) {
			position.X *= reference.X;
			position.Y *= reference.Y;
		}
		const auto spacing = c.Vec2("spacing", {1, 1});
		if (spacing.X == 0 || spacing.Y == 0)
			return c.Fail(Status::UnsupportedExecution, "Dotted spacing divides by zero", "spacing");
		const double minimum = std::min(size.Range.X, size.Range.Y);
		const auto extent = [&](double side, double gap) {
			if (minimum == 0) return 16.;
			return std::min(16., std::ceil(side / minimum / gap / gap));
		};
		const Vector2 span{extent(dimension.X, spacing.X), extent(dimension.Y, spacing.Y)};
		if (!DottedFinite(c, span, "size")) return false;
		const double intensity = c.Scalar("intensity", 1), aa = c.Scalar("smoothness", .1);
		if (render == 1 && !(aa > 0))
			return c.Fail(
				Status::UnsupportedExecution,
				"Dotted AA smoothstep has unordered or equal edges",
				"smoothness"
			);
		const auto background = source2d::InputColour(c, "bg_color", {0, 0, 0, 255}),
				   solid = source2d::InputColour(c, "dot_color", {255, 255, 255, 255});
		const Value *paletteValue = c.Find("palette"), *gradientValue = c.Find("gradient");
		const auto *palette = paletteValue ? std::get_if<ArrayValue>(paletteValue) : nullptr;
		const auto *gradient = gradientValue ? std::get_if<Gradient>(gradientValue) : nullptr;
		if (colourMode == 1 && (!palette || palette->Elements.empty() || palette->Elements.size() > 256 ||
								!palette->Nested.empty() || !palette->Items.empty()))
			return c.Fail(
				Status::UnsupportedExecution, "Dotted GLSL palette requires 1 to 256 flat colours", "palette"
			);
		if (colourMode == 1)
			for (const auto &v : palette->Elements)
				if (!std::holds_alternative<Colour>(v))
					return c.Fail(Status::TypeMismatch, "Dotted palette requires colours", "palette");
		if (colourMode == 2 &&
			(!gradient || gradient->Keys.empty() || gradient->Keys.size() > GRADIENT_KEY_SLOTS))
			return c.Fail(
				Status::UnsupportedExecution, "Dotted GLSL gradient requires 1 to 64 keys", "gradient"
			);
		const Image *texture = c.Input("texture");
		if (colourMode == 3 && !texture)
			return c.Fail(
				Status::UnsupportedExecution,
				"Dotted Texture mode requires an observed texture sampler",
				"texture"
			);
		if (colourMode == 2 && !c.Find("seed"))
			return c.Fail(
				Status::UnsupportedExecution, "Dotted Random mode requires the authored source seed", "seed"
			);
		const double seed = c.Scalar("seed"), shift = c.Scalar("shift");
		Image *output = c.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				const double amount = DottedSample(size, u, v),
							 radians = DottedSample(angle, u, v) * std::numbers::pi / 180;
				if (amount == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Dotted selected Size divides by zero", "size"
					);
				Vector2 amounts{dimension.X / amount / spacing.X, dimension.Y / amount / spacing.Y};
				double uvAlpha = 1;
				const auto uv = source2d::GeneratorUv(c, u, v, uvAlpha);
				const auto pos = source2d::Rotate(
					{uv.X - position.X / dimension.X, uv.Y - position.Y / dimension.Y}, radians
				);
				constexpr double HEX = 0.86602540378443864676;
				if (pattern == 1) amounts.Y /= HEX;
				if (!DottedFinite(c, amounts, "size")) return false;
				Vector2 box{std::floor(pos.X * amounts.X), std::floor(pos.Y * amounts.Y)};
				if (!DottedFinite(c, box, "angle")) return false;
				if (pattern == 1 && std::floor(DottedMod(box.Y, 2)) == 1) box.X += .5;
				Vector2 fraction{pos.X * amounts.X - box.X, pos.Y * amounts.Y - box.Y};
				if (pattern == 1) fraction.Y = .5 + (fraction.Y - .5) * HEX;
				const Vector2 center{box.X / amounts.X, box.Y / amounts.Y};
				if (!DottedFinite(c, center, "size")) return false;
				Rgba dots{};
				for (double i = -span.X; i <= span.X; ++i)
					for (double j = -span.Y; j <= span.Y; ++j) {
						const double ii = i + (pattern == 1 && DottedMod(j, 2) == 1 ? .5 : 0);
						const Vector2 offset{ii, pattern == 1 ? j * HEX : j};
						const Vector2 tx{center.X + offset.X / amounts.X, center.Y + offset.Y / amounts.Y};
						if (!DottedFinite(c, tx, "size")) return false;
						Rgba colour{1, 1, 1, 1};
						if (colourMode == 1) {
							const double index = DottedMod(
								tx.Y * amounts.Y + tx.X * amounts.X, double(palette->Elements.size())
							);
							if (!std::isfinite(index) || index < 0 || index >= palette->Elements.size())
								return c.Fail(
									Status::UnsupportedExecution,
									"Dotted palette index is undefined",
									"palette"
								);
							const auto &p = std::get<Colour>(palette->Elements[size_t(index)]);
							colour = {p.Red / 255., p.Green / 255., p.Blue / 255., p.Alpha / 255.};
						} else if (colourMode == 2) {
							const double random = ShaderFract(
								std::sin(tx.X * amounts.X * 1892.9898 + tx.Y * amounts.Y * 78.23453) *
								DottedMod(seed + 437.54123, 100000) / 10
							);
							colour = GradientEval(
								GradientSampler{gradient}, ShaderFract(ShaderFract(random + shift) + 1)
							);
						} else if (colourMode == 3)
							colour = SampleNearest(*texture, tx.X, tx.Y);
						const double threshold = DottedSample(
							dot, tx.X + position.X / dimension.X, tx.Y + position.Y / dimension.Y
						);
						const double distance = std::hypot(
													(fraction.X - offset.X) * spacing.X - .5,
													(fraction.Y - offset.Y) * spacing.Y - .5
												) *
												2;
						double coverage = 0;
						if (render == 0) coverage = 1 - distance >= 1 - threshold ? 1 : 0;
						if (render == 1) {
							const double t = std::clamp((1 - distance - (1 - aa - threshold)) / aa, 0., 1.);
							coverage = t * t * (3 - 2 * t);
						}
						if (render == 2 && threshold != 0) coverage = std::max(0., threshold - distance);
						for (size_t channel = 0; channel < 4; ++channel)
							dots[channel] += colour[channel] * intensity * coverage;
					}
				const Rgba foreground =
					colourMode == 0 ? Rgba{solid[0], solid[1], solid[2], solid[3] * dots[3]} : dots;
				Rgba result{};
				if (blend == 0) {
					const double alpha = foreground[3] + background[3] * (1 - foreground[3]);
					if (alpha != 0) {
						for (size_t channel = 0; channel < 3; ++channel)
							result[channel] = (foreground[channel] * foreground[3] +
											   background[channel] * background[3] * (1 - foreground[3])) /
											  alpha;
						result[3] = alpha;
					}
				} else
					for (size_t channel = 0; channel < 4; ++channel)
						result[channel] = background[channel] + foreground[channel];
				result[3] *= uvAlpha;
				if (!WritePixel(*output, x, y, result))
					return c.Fail(
						Status::UnsupportedExecution,
						"Dotted shader sample is nonfinite or exceeds surface range",
						"surface_out"
					);
				if (mask) {
					// mask_apply_empty draws through RGBA8; safe single-red drawing replaces its mask shader.
					result = SourceSafeDrawPixel(*output, x, y);
					const bool red = DescribeSurfaceFormat(*format)->Channels == 1;
					if (!red) {
						const auto m = SampleNearest(*mask, u, v);
						result[3] *= (m[0] + m[1] + m[2]) / 3 * m[3];
					}
					for (auto &channel : result)
						channel = Quantize(channel) / 255.;
					if (!WritePixel(*output, x, y, result))
						return c.Fail(
							Status::InvalidValue, "Dotted masked sample exceeds surface range", "surface_out"
						);
				}
			}
		return c.FailureCode == Status::Ok;
	}
}
