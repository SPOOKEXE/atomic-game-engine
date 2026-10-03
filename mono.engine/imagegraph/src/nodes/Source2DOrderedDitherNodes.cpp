#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "ColorSpace.hpp"
#include "Processor.hpp"
#include "SourceInterpret.hpp"

#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		const Value *OrderedOriginalInput(const NodeContext &context, std::string_view port) {
			for (auto i = context.ProcessorOriginalValues.rbegin();
				 i != context.ProcessorOriginalValues.rend();
				 ++i)
				if (i->first == port) return i->second;
			for (const auto &[id, value] : context.Values)
				if (id == port) return &value;
			return context.Find(port);
		}
		struct OrderedNumericBounds {
			double Minimum = std::numeric_limits<double>::infinity(),
				   Maximum = -std::numeric_limits<double>::infinity();
			bool Found = false;
		};
		template <class Leaf> void OrderedBoundNumber(const Leaf &leaf, OrderedNumericBounds &out) {
			const auto add = [&](double n) {
				out.Found = true;
				out.Minimum = std::min(out.Minimum, n);
				out.Maximum = std::max(out.Maximum, n);
				if (!std::isfinite(n)) out.Maximum = std::numeric_limits<double>::infinity();
			};
			if (const auto *n = std::get_if<double>(&leaf))
				add(*n);
			else if (const auto *n = std::get_if<int64_t>(&leaf))
				add(double(*n));
			else if (const auto *n = std::get_if<EnumValue>(&leaf))
				add(double(n->Value));
			else if (const auto *n = std::get_if<bool>(&leaf))
				add(*n ? 1 : 0);
		}
		OrderedNumericBounds
		OrderedAllNumbers(const NodeContext &context, std::string_view port, double fallback) {
			OrderedNumericBounds out;
			if (const Value *value = OrderedOriginalInput(context, port)) {
				if (const auto *array = std::get_if<ArrayValue>(value)) {
					for (const auto &leaf : array->Elements)
						OrderedBoundNumber(leaf, out);
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							OrderedBoundNumber(leaf, out);
					const auto visit =
						[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> void {
						if (depth > Limits::MaximumArrayDepth) return;
						for (const auto &item : items) {
							if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
								OrderedBoundNumber(*leaf, out);
							else if (const auto *nested =
										 std::get_if<std::vector<SourceArrayItem>>(&item.Data))
								self(self, *nested, depth + 1);
						}
					};
					visit(visit, array->Items, 0);
				} else
					OrderedBoundNumber(*value, out);
			}
			if (!out.Found) {
				out.Minimum = out.Maximum = fallback;
			}
			return out;
		}
		uint64_t OrderedLargestImagePixels(const NodeContext &context, std::string_view port) {
			uint64_t pixels = 0;
			if (const Image *image = context.Input(port)) pixels = uint64_t(image->Width) * image->Height;
			for (const auto &[id, images] : context.ImageArrays)
				if (id == port && images)
					for (const auto &image : images->Images)
						pixels = std::max(pixels, uint64_t(image.Width) * image.Height);
			return pixels;
		}

		size_t OrderedPaletteExtent(const Value *value) {
			if (!value) return 2;
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array) return 1;
			size_t count = array->Elements.size();
			for (const auto &row : array->Nested)
				count = std::max(count, row.size());
			const auto visit =
				[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> size_t {
				if (depth > Limits::MaximumArrayDepth) return 257;
				size_t maximum = items.size();
				for (const auto &item : items)
					if (const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
						maximum = std::max(maximum, self(self, *row, depth + 1));
				return maximum;
			};
			return std::max(count, visit(visit, array->Items, 0));
		}
		bool OrderedAdmitBatch(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			const auto active = OrderedAllNumbers(c, "active", 1);
			if (active.Minimum == 0 && active.Maximum == 0) return true;
			const uint64_t rows = std::max(uint64_t{1}, uint64_t(c.ProcessorCount));
			const auto colour = OrderedAllNumbers(c, "color_type", 1);
			const bool palette = std::floor(colour.Minimum) <= 1 && std::ceil(colour.Maximum) >= 1;
			const size_t extent = palette ? OrderedPaletteExtent(OrderedOriginalInput(c, "palette")) : 0;
			if (extent > 256)
				return c.Fail(
					Status::LimitExceeded, "Dither palette exceeds source GLSL uniform slots", "palette"
				);
			const uint64_t cost = 96 + extent * 48, pixels = OrderedLargestImagePixels(c, "surface_in");
			if (rows > 64000000 / cost || pixels > 64000000 / (rows * cost))
				return c.Fail(
					Status::LimitExceeded, "Dither complete processor batch exceeds work budget", "surface_in"
				);
			const uint64_t maskPixels = OrderedLargestImagePixels(c, "mask");
			const double feather = OrderedAllNumbers(c, "mask_feather", 0).Maximum;
			if (maskPixels && feather > 0) {
				if (!std::isfinite(feather) || feather > 64)
					return c.Fail(
						Status::LimitExceeded, "Dither mask feather exceeds work budget", "mask_feather"
					);
				const uint64_t maskCost = 16 + uint64_t(std::ceil(feather)) * 16;
				if (maskPixels > (64000000 / rows - pixels * cost) / maskCost)
					return c.Fail(
						Status::LimitExceeded,
						"Dither mask exceeds complete batch work budget",
						"mask_feather"
					);
			}
			return true;
		}
		struct OrderedPalette {
			const ArrayValue *Array = nullptr;
			const Value *Scalar = nullptr;
			size_t Count = 0;
			std::optional<Colour> At(size_t index) const {
				if (index >= Count) return {};
				if (Scalar)
					return std::visit([](const auto &v) { return InterpretPackedColour(v); }, *Scalar);
				const ElementValue *leaf = Array->Items.empty()
											   ? &Array->Elements[index]
											   : std::get_if<ElementValue>(&Array->Items[index].Data);
				return leaf ? std::visit([](const auto &v) { return InterpretPackedColour(v); }, *leaf)
							: std::optional<Colour>{};
			}
		};
		Rgba OrderedColour(Colour c) {
			return {c.Red / 255., c.Green / 255., c.Blue / 255., c.Alpha / 255.};
		}
		Rgb3 OrderedLab(Rgb3 rgb) {
			for (auto &v : rgb)
				v = v > .04045 ? std::pow((v + .055) / 1.055, 2.4) : v / 12.92;
			Rgb3 xyz{
				100 * (rgb[0] * .4124 + rgb[1] * .3576 + rgb[2] * .1805),
				100 * (rgb[0] * .2126 + rgb[1] * .7152 + rgb[2] * .0722),
				100 * (rgb[0] * .0193 + rgb[1] * .1192 + rgb[2] * .9505)
			};
			constexpr Rgb3 white{95.047, 100, 108.883};
			for (size_t i = 0; i < 3; ++i) {
				double n = xyz[i] / white[i];
				xyz[i] = n > .008856 ? std::pow(n, 1. / 3) : 7.787 * n + 16. / 116;
			}
			return {
				(116 * xyz[1] - 16) / 100.,
				.5 + .5 * (500 * (xyz[0] - xyz[1]) / 127.),
				.5 + .5 * (200 * (xyz[1] - xyz[2]) / 127.)
			};
		}
		double OrderedLabDistance(const Rgba &a, const Rgba &b) {
			const auto x = OrderedLab({a[0], a[1], a[2]}), y = OrderedLab({b[0], b[1], b[2]});
			double d = 0;
			for (size_t i = 0; i < 3; ++i)
				d += (x[i] - y[i]) * (x[i] - y[i]);
			return std::sqrt(d);
		}
		uint32_t OrderedBayer(uint32_t x, uint32_t y, uint32_t side) {
			constexpr std::array<uint32_t, 4> base{0, 2, 3, 1};
			if (side == 2) return base[(y % 2) * 2 + x % 2];
			return 4 * OrderedBayer(x % (side / 2), y % (side / 2), side / 2) +
				   base[(y / (side / 2)) * 2 + x / (side / 2)];
		}
		double OrderedMod(double x, double y) {
			return x - std::floor(x / y) * y;
		}
		bool OrderedSingleRed(const Image &image) {
			return image.Format == SurfaceFormat::R8Unorm || image.Format == SurfaceFormat::R16Float ||
				   image.Format == SurfaceFormat::R32Float;
		}
	}
	bool SourceOrderedDither(NodeContext &c) {
		if (!OrderedAdmitBatch(c)) return false;
		bool failed = false;
		if (CopyWhenInactive(c, failed)) return !failed;
		const Image *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "Dither requires its surface", "surface_in");
		if (OrderedSingleRed(*source))
			return RunPixelProcessor(c, [](const Image &image, uint32_t x, uint32_t y, double, double) {
				return SourceSafeDrawPixel(image, x, y);
			});
		if (c.Integer("mode") != 0)
			return c.Fail(
				Status::UnsupportedExecution,
				"Dither Alpha retains uncaptured colour-pass shader uniforms",
				"mode"
			);
		const int64_t pattern = c.Integer("pattern"), mapMode = c.Integer("map_mode"),
					  colourType = c.Integer("color_type");
		if (pattern < 0 || pattern > 5 || mapMode < 0 || mapMode > 1 || colourType < 0 || colourType > 3)
			return c.Fail(Status::InvalidValue, "Dither source choice is invalid", "pattern");
		const Vector2 scale = c.Vec2("dither_scale", {1, 1});
		if (!std::isfinite(scale.X) || !std::isfinite(scale.Y) || scale.X == 0 || scale.Y == 0)
			return c.Fail(
				Status::UnsupportedExecution,
				"Dither zero or nonfinite scale has undefined shader division",
				"dither_scale"
			);
		const bool invert = c.Boolean("invert");
		const Image *map = c.Input("dither_map");
		const MatrixValue *matrix = nullptr;
		uint32_t columns = pattern <= 2 ? uint32_t(2u << pattern) : 2, rows = columns;
		if (pattern == 5) {
			const Value *v = c.Find("dither_matrix");
			matrix = v ? std::get_if<MatrixValue>(v) : nullptr;
			if (!matrix)
				return c.Fail(
					Status::TypeMismatch, "Dither Matrix needs a typed numeric matrix", "dither_matrix"
				);
			columns = matrix->Columns;
			rows = matrix->Rows;
			if (!columns || !rows || uint64_t(columns) * rows > 64 ||
				matrix->Values.size() != uint64_t(columns) * rows)
				return c.Fail(
					Status::LimitExceeded,
					"Dither matrix exceeds its 64 shader uniform slots",
					"dither_matrix"
				);
			if (mapMode == 0 && uint64_t(columns) * rows <= 1)
				return c.Fail(
					Status::UnsupportedExecution, "Dither single-cell matrix divides by zero", "dither_matrix"
				);
		}
		OrderedPalette palette;
		if (colourType == 1) {
			const Value *v = c.Find("palette");
			if (!v) return c.Fail(Status::InvalidValue, "Dither palette is missing", "palette");
			if (const auto *a = std::get_if<ArrayValue>(v)) {
				if (!a->Nested.empty())
					return c.Fail(Status::TypeMismatch, "Dither palette row must be flat", "palette");
				palette = {a, nullptr, a->Items.empty() ? a->Elements.size() : a->Items.size()};
			} else
				palette = {nullptr, v, 1};
			if (palette.Count == 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Dither empty palette reads uncaptured shader uniforms",
					"palette"
				);
			if (palette.Count > 256)
				return c.Fail(
					Status::LimitExceeded, "Dither palette exceeds source GLSL uniform slots", "palette"
				);
			for (size_t i = 0; i < palette.Count; ++i)
				if (!palette.At(i))
					return c.Fail(
						Status::UnsupportedExecution,
						"Dither palette contains unresolved packed colours",
						"palette"
					);
		}
		const double steps = double(c.Integer("steps", 4));
		const Rgb3 rgbSteps{
			double(c.Integer("r_steps", 4)), double(c.Integer("g_steps", 4)), double(c.Integer("b_steps", 4))
		};
		if ((colourType == 0 && steps == 0) || ((colourType == 2 || colourType == 3) &&
												(rgbSteps[0] == 0 || rgbSteps[1] == 0 || rgbSteps[2] == 0)))
			return c.Fail(Status::UnsupportedExecution, "Dither zero steps divide by zero", "steps");
		if (pattern == 3 && !c.Find("seed"))
			return c.Fail(Status::InvalidValue, "Dither White Noise requires its resolved seed", "seed");
		const double seed = c.Scalar("seed");
		const bool mapped = c.Boolean("contrast_mapped");
		const Image *contrastMap = mapped ? c.Input("contrast_map") : nullptr;

		Vector2 contrast{};
		if (mapped) {
			if (!ReadSourceMappedRange(c, "contrast", contrast)) return false;
		} else {
			const auto scalar = c.Scalar("contrast", 1);
			contrast = {scalar, scalar};
		}
		return RunPixelProcessor(c, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			double pu = u, pv = v;
			if (scale.X != 1 || scale.Y != 1) {
				pu = std::floor(u / (scale.X / image.Width)) * (scale.X / image.Width);
				pv = std::floor(v / (scale.Y / image.Height)) * (scale.Y / image.Height);
			}
			Rgba input = SampleNearest(image, pu, pv), first{}, second{};
			bool exact = false;
			if (colourType == 1) {
				size_t i1 = 0, i2 = 0;
				double d1 = 99, d2 = 99;
				for (size_t i = 0; i < palette.Count; ++i) {
					const auto pc = OrderedColour(*palette.At(i));
					const double d = OrderedLabDistance(pc, input);
					if (d <= .001) {
						exact = true;
						for (size_t j = 0; j < 3; ++j)
							input[j] = pc[j];
					} else if (d < d1) {
						d2 = d1;
						i2 = i1;
						d1 = d;
						i1 = i;
					} else if (d < d2) {
						d2 = d;
						i2 = i;
					}
				}
				first = OrderedColour(*palette.At(i1));
				second = OrderedColour(*palette.At(i2));
			} else {
				const Rgb3 components = colourType == 3 ? ShaderRgbToHsv({input[0], input[1], input[2]})
														: Rgb3{input[0], input[1], input[2]};
				for (size_t j = 0; j < 3; ++j) {
					const double n = colourType == 0 ? steps : rgbSteps[j];
					first[j] = std::floor(components[j] * n) / n;
					second[j] = std::ceil(components[j] * n) / n;
				}
				if (colourType == 3) {
					const auto a = ShaderHsvToRgb({first[0], first[1], first[2]}),
							   b = ShaderHsvToRgb({second[0], second[1], second[2]});
					for (size_t j = 0; j < 3; ++j) {
						first[j] = a[j];
						second[j] = b[j];
					}
				}
				first[3] = second[3] = input[3];
				double distance = 0;
				for (size_t j = 0; j < 3; ++j)
					distance += (input[j] - first[j]) * (input[j] - first[j]);
				exact = std::sqrt(distance) < .05;
			}
			if (exact) {
				input[3] *= input[3];
				return input;
			}
			const double d1 = OrderedLabDistance(input, first), d2 = OrderedLabDistance(input, second);
			if (!std::isfinite(d1 + d2) || d1 + d2 == 0) {
				c.Fail(Status::UnsupportedExecution, "Dither colour ratio is undefined", "color_type");
				return Rgba{};
			}
			double con = contrast.X;
			if (contrastMap) {
				auto p = SampleNearest(*contrastMap, u, v);
				con = contrast.X + (contrast.Y - contrast.X) * (p[0] + p[1] + p[2]) / 3;
			}
			const double ratio = (d1 / (d1 + d2) - .5) * con + .5;
			double threshold = 0;
			if (pattern == 3) {
				const double random =
					std::sin(u * 1892.9898 + v * 78.23453) * OrderedMod(seed + 437.54123, 100000.) / 10.;
				threshold = random - std::floor(random);
			} else if (pattern == 4) {
				if (!map) {
					c.Fail(
						Status::UnsupportedExecution,
						"Dither Custom without a map retains uncaptured pattern uniforms",
						"dither_map"
					);
					return Rgba{};
				}
				if (mapMode == 1) {
					c.Fail(
						Status::UnsupportedExecution,
						"Dither Custom Linear reads an uncaptured dither array",
						"map_mode"
					);
					return Rgba{};
				}
				const auto p = SampleNearest(
					*map,
					OrderedMod(std::floor(pu * image.Width), map->Width) / map->Width,
					OrderedMod(std::floor(pv * image.Height), map->Height) / map->Height
				);
				threshold = p[0] * .2126 + p[1] * .7152 + p[2] * .0722;
			} else {
				const size_t amount = size_t(columns) * rows;
				size_t index = 0;
				if (mapMode == 1) {
					const double selected = std::trunc(ratio * amount);
					if (!std::isfinite(selected) || selected < 0 || selected >= amount) {
						c.Fail(
							Status::UnsupportedExecution,
							"Dither Linear index reads outside the uploaded shader array",
							"map_mode"
						);
						return Rgba{};
					}
					index = size_t(selected);
				} else
					index = size_t(OrderedMod(std::floor(pv * image.Height), rows)) * columns +
							size_t(OrderedMod(std::floor(pu * image.Width), columns));
				threshold =
					matrix
						? matrix->Values[index]
						: double(OrderedBayer(uint32_t(index % columns), uint32_t(index / columns), columns));
				if (mapMode == 0) threshold /= double(amount - 1);
			}
			if (pattern != 3 && invert) threshold = 1 - threshold;
			Rgba out = ratio < threshold ? first : second;
			out[3] *= input[3];
			return out;
		});
	}
} // namespace engine::imagegraph::detail
