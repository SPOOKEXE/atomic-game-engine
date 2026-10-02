#include "Families.hpp"
#include "SourceInterpret.hpp"

namespace engine::imagegraph::detail {
	namespace {
		struct ColourFilterPalette {
			const Value *Scalar = nullptr;
			const ArrayValue *Array = nullptr;
			size_t Count = 0;
			std::optional<Colour> At(size_t index) const {
				if (index >= Count) return std::nullopt;
				if (Scalar)
					return std::visit([](const auto &v) { return InterpretPackedColour(v); }, *Scalar);
				const auto *leaf = Array->Items.empty()
									   ? &Array->Elements[index]
									   : std::get_if<ElementValue>(&Array->Items[index].Data);
				return leaf ? std::visit([](const auto &v) { return InterpretPackedColour(v); }, *leaf)
							: std::optional<Colour>{};
			}
		};
		bool ReadColourFilterPalette(
			NodeContext &c,
			std::string_view port,
			ColourFilterPalette &palette,
			size_t uniformLimit = 256,
			bool truncatePalette = false
		) {
			const auto *value = c.Find(port);
			if (!value) return c.Fail(Status::InvalidValue, "Colour palette input is missing", port);
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Nested.empty())
					return c.Fail(Status::UnsupportedExecution, "Colour palette row must be flat", port);
				palette = {
					nullptr, array, array->Items.empty() ? array->Elements.size() : array->Items.size()
				};
			} else
				palette = {value, nullptr, 1};
			if (truncatePalette) palette.Count = std::min(palette.Count, uniformLimit);
			if (palette.Count > uniformLimit)
				return c.Fail(
					Status::LimitExceeded, "Colour palette exceeds source GLSL uniform slots", port
				);
			for (size_t i = 0; i < palette.Count; ++i)
				if (!palette.At(i))
					return c.Fail(
						Status::UnsupportedExecution,
						"Colour palette contains unresolved packed colours",
						port
					);
			return true;
		}
		Rgba FilterColour(Colour c) {
			return {c.Red / 255., c.Green / 255., c.Blue / 255., c.Alpha / 255.};
		}
		Rgb3 FilterHsv(const Rgba &c) {
			return ShaderRgbToHsv({c[0], c[1], c[2]});
		}
		double FilterDistance(const Rgb3 &a, const Rgb3 &b) {
			double n = 0;
			for (size_t i = 0; i < 3; ++i)
				n += (a[i] - b[i]) * (a[i] - b[i]);
			return std::sqrt(n);
		}
		Rgb3 FilterLab(Rgb3 rgb) {
			for (auto &v : rgb)
				v = v > .04045 ? std::pow((v + .055) / 1.055, 2.4) : v / 12.92;
			Rgb3 xyz{
				100 * (rgb[0] * .4124 + rgb[1] * .3576 + rgb[2] * .1805),
				100 * (rgb[0] * .2126 + rgb[1] * .7152 + rgb[2] * .0722),
				100 * (rgb[0] * .0193 + rgb[1] * .1192 + rgb[2] * .9505)
			};
			constexpr Rgb3 white{95.047, 100, 108.883};
			for (size_t i = 0; i < 3; ++i) {
				auto n = xyz[i] / white[i];
				xyz[i] = n > .008856 ? std::pow(n, 1. / 3.) : 7.787 * n + 16. / 116.;
			}
			return {
				(116 * xyz[1] - 16) / 100.,
				.5 + .5 * (500 * (xyz[0] - xyz[1]) / 127.),
				.5 + .5 * (200 * (xyz[1] - xyz[2]) / 127.)
			};
		}
		bool ColourReplace(NodeContext &c) {
			bool failed = false;
			if (CopyWhenInactive(c, failed)) return !failed;
			const auto *source = c.Input("surface_in");
			if (!source)
				return c.Fail(Status::InvalidValue, "Replace Palette requires a surface", "surface_in");
			const int64_t mode = c.Integer("mode");
			// array_shuffle is an opaque source builtin; its seeded permutation requires
			// an observation.
			if (mode == 1) return ReplayRecordedHostOutputs(c);
			ColourFilterPalette from, to;
			if (!ReadColourFilterPalette(c, "from", from) || !ReadColourFilterPalette(c, "to", to))
				return false;
			if (!from.Count || !to.Count || (mode == 0 && from.Count == 1))
				return ReplayRecordedHostOutputs(c);
			const double threshold = c.Scalar("threshold", .1), seed = c.Scalar("seed"),
						 hueRan = c.Scalar("hue_randomize"), satRan = c.Scalar("sat_randomize"),
						 valRan = c.Scalar("val_randomize");
			for (double value : {threshold, hueRan, satRan, valRan})
				if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
					return c.Fail(
						Status::InvalidValue,
						"Replace Palette controls must fit finite shader floats",
						"threshold"
					);
			if (mode == 2 && (!std::isfinite(seed) || std::abs(seed) > std::numeric_limits<float>::max()))
				return c.Fail(
					Status::InvalidValue, "Replace Palette Seed must fit a finite shader float", "seed"
				);
			if (mode == 2 && (hueRan != 0 || satRan != 0 || valRan != 0) && !c.Find("seed"))
				return ReplayRecordedHostOutputs(c);
			const bool others = c.Boolean("replace_other_colors"), hard = c.Boolean("hard_replace", true),
					   alpha = c.Boolean("multiply_alpha", true);
			const auto replacement = FilterColour(c.Get<Colour>("target_color", Colour{0, 0, 0, 255}));
			const auto format = ResolveProcessorSurfaceFormat(c, source);
			if (!format) return false;
			auto *output = c.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			const auto random = [&](double value) {
				const double divisor = 43758.5453123 + seed;
				return ShaderFract(
					std::sin(value * 12.9898 + 53.4856) *
					(divisor - std::floor(divisor / 100000.) * 100000.) / 10.
				);
			};
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x) {
					const auto col = ReadPixel(*source, x, y);
					auto base = others ? replacement : col;
					auto hsv = FilterHsv(col);
					if (alpha)
						for (auto &v : hsv)
							v *= col[3];
					if (others)
						if (const auto *mask = c.Input("mask")) {
							const auto m =
								SampleNearest(*mask, (x + .5) / source->Width, (y + .5) / source->Height);
							if ((m[0] + m[1] + m[2]) * m[3] < .5) {
								if (!WritePixel(*output, x, y, base))
									return c.Fail(
										Status::InvalidValue,
										"Replace Palette sample is nonfinite",
										"surface_out"
									);
								continue;
							}
						}
					double nearest = threshold;
					size_t fi = 0, ti = 0;
					for (size_t i = 0; i < from.Count; ++i) {
						const double d = FilterDistance(hsv, FilterHsv(FilterColour(*from.At(i))));
						if (d < nearest) {
							nearest = d;
							fi = i;
						}
					}
					if (mode == 0)
						ti = size_t(
							std::floor(double(fi) / double(from.Count - 1) * double(to.Count - 1) + .5)
						);
					else if (mode == 2) {
						const auto fh = FilterHsv(FilterColour(*from.At(fi)));
						double best = 999999.;
						for (size_t i = 0; i < to.Count; ++i) {
							auto th = FilterHsv(FilterColour(*to.At(i)));
							Rgb3 delta{
								fh[0] - th[0] + (random(fh[0]) - .5) * 2 * hueRan,
								fh[1] - th[1] + (random(fh[1]) - .5) * 2 * satRan,
								fh[2] - th[2] + (random(fh[2]) - .5) * 2 * valRan
							};
							const double d = FilterDistance(delta, {});
							if (d < best) {
								best = d;
								ti = i;
							}
						}
					}
					auto result = base;
					if (nearest < threshold) {
						result = FilterColour(*to.At(ti));
						if (!hard) {
							const double amount = nearest / threshold;
							for (size_t i = 0; i < 4; ++i)
								result[i] = base[i] * amount + result[i] * (1 - amount);
						}
					}
					if (!others) result[3] = col[3];
					if (!WritePixel(*output, x, y, result))
						return c.Fail(
							Status::InvalidValue, "Replace Palette sample is nonfinite", "surface_out"
						);
				}
			FinishProcessor(c, *source, *output);
			return c.FailureCode == Status::Ok;
		}
		bool ColoursReplace(NodeContext &c) {
			bool failed = false;
			if (CopyWhenInactive(c, failed)) return !failed;
			ColourFilterPalette from, to;
			if (!ReadColourFilterPalette(c, "palette_from", from, 256, true) ||
				!ReadColourFilterPalette(c, "palette_to", to, 256, true))
				return false;
			// shader_set_palette leaves empty/unsubmitted slots unchanged; index beyond
			// To is not invented.
			if (!from.Count || to.Count < from.Count) return ReplayRecordedHostOutputs(c);
			return RunPixelProcessor(c, [&](const Image &, uint32_t x, uint32_t y, double, double) {
				const auto col = ReadPixel(*c.Input("surface_in"), x, y);
				double best = 999.;
				size_t index = 0;
				for (size_t i = 0; i < from.Count; ++i) {
					const auto fr = FilterColour(*from.At(i));
					const double d = FilterDistance({col[0], col[1], col[2]}, {fr[0], fr[1], fr[2]});
					if (d < best) {
						best = d;
						index = i;
					}
				}
				auto result = FilterColour(*to.At(index));
				result[3] *= col[3];
				return result;
			});
		}
		uint8_t ColourFilterByte(double value) {
			value = std::clamp(value, 0., 255.);
			const double lower = std::floor(value), part = value - lower;
			return uint8_t(lower + (part > .5 || (part == .5 && std::fmod(lower, 2) != 0)));
		}
		Rgb3 ColourFilterHsl(const Rgb3 &rgb) {
			const double low = std::min({rgb[0], rgb[1], rgb[2]}), high = std::max({rgb[0], rgb[1], rgb[2]}),
						 l = (high + low) / 2, d = high - low;
			if (!d) return {0, 0, l};
			double h = rgb[0] == high	? (rgb[1] - rgb[2]) / d
					   : rgb[1] == high ? 2 + (rgb[2] - rgb[0]) / d
										: 4 + (rgb[0] - rgb[1]) / d;
			if (h < 0) h += 6;
			return {h / 6, l < .5 ? d / (high + low) : d / (2 - high - low), l};
		}
		Rgb3 ColourFilterFromHsl(const Rgb3 &hsl) {
			if (hsl[1] == 0) return {hsl[2], hsl[2], hsl[2]};
			const double m2 = hsl[2] <= .5 ? hsl[2] * (1 + hsl[1]) : hsl[2] + hsl[1] - hsl[2] * hsl[1],
						 m1 = 2 * hsl[2] - m2;
			const auto hue = [&](double h) {
				if (h < 0)
					h += 1;
				else if (h > 1)
					h -= 1;
				if (6 * h < 1) return m1 + (m2 - m1) * h * 6;
				if (2 * h < 1) return m2;
				if (3 * h < 2) return m1 + (m2 - m1) * (2. / 3. - h) * 6;
				return m1;
			};
			return {hue(hsl[0] + 1. / 3.), hue(hsl[0]), hue(hsl[0] - 1. / 3.)};
		}
		Rgb3 ColourFilterBlend(const Rgb3 &rgb, const Rgb3 &blend, double amount, int64_t mode) {
			Rgb3 target = blend;
			const auto hsv = ShaderRgbToHsv(rgb), bhsv = ShaderRgbToHsv(blend);
			const double lum = rgb[0] * .2126 + rgb[1] * .7152 + rgb[2] * .0722;
			if (mode == 6)
				target = ShaderHsvToRgb({bhsv[0], hsv[1], hsv[2]});
			else if (mode == 7)
				return ShaderHsvToRgb({hsv[0], hsv[1] + (bhsv[1] - hsv[1]) * amount, hsv[2]});
			else if (mode == 8) {
				auto hsl = ColourFilterHsl(rgb);
				const auto b = ColourFilterHsl(blend);
				hsl[2] += (b[2] - hsl[2]) * amount;
				return ColourFilterFromHsl(hsl);
			} else
				for (size_t i = 0; i < 3; ++i) {
					const double a = rgb[i], b = blend[i];
					switch (mode) {
					case 1:
						target[i] = a + b;
						break;
					case 2:
						target[i] = a - b;
						break;
					case 3:
						target[i] = a * b;
						break;
					case 4:
						target[i] = 1 - (1 - a) * (1 - b);
						break;
					case 5:
						target[i] = lum > .5 ? 1 - (1 - 2 * (a - .5)) * (1 - b) : 2 * a * b;
						break;
					case 9:
						target[i] = std::max(a, b);
						break;
					case 10:
						target[i] = std::min(a, b);
						break;
					case 12:
						target[i] = std::abs(a - b);
						break;
					default:
						break;
					}
				}
			Rgb3 result{};
			for (size_t i = 0; i < 3; ++i)
				result[i] = rgb[i] + (target[i] - rgb[i]) * amount;
			return result;
		}
		double ColourFilterNumericControl(const NodeContext &c, std::string_view id) {
			if (const auto *value = c.Find(id)) {
				if (const auto *array = std::get_if<ArrayValue>(value)) {
					const ElementValue *leaf = nullptr;
					if (!array->Nested.empty() || (array->Items.empty() && array->Elements.empty())) {
						c.Fail(
							Status::UnsupportedExecution,
							"Color Adjust control has no numeric first element",
							id
						);
						return 0;
					}
					if (!array->Items.empty())
						leaf = std::get_if<ElementValue>(&array->Items.front().Data);
					else
						leaf = &array->Elements.front();
					if (!leaf) {
						c.Fail(
							Status::UnsupportedExecution, "Color Adjust control first element is nested", id
						);
						return 0;
					}
					return std::visit(
						[&](const auto &raw) -> double {
							using T = std::decay_t<decltype(raw)>;
							if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
										  std::is_same_v<T, bool>)
								return double(raw);
							else {
								c.Fail(
									Status::InvalidValue,
									"Color Adjust control row must start with a number",
									id
								);
								return 0;
							}
						},
						*leaf
					);
				}
				if (const auto *vector = std::get_if<Vector2>(value)) return vector->X;
			}
			return c.Scalar(id);
		}
		double
		ColourFilterControl(const NodeContext &c, std::string_view id, double u, double v, bool palette) {
			const std::string base(id), rangeId = base + "_map_range";
			const auto authored = [&](std::string_view port) {
				return std::any_of(
						   c.Authored.Values.begin(),
						   c.Authored.Values.end(),
						   [&](const AuthoredValue &value) { return value.Port == port; }
					   ) ||
					   std::any_of(c.ValueViews.begin(), c.ValueViews.end(), [&](const auto &binding) {
						   return binding.first == port && binding.second;
					   });
			};
			const double value = ColourFilterNumericControl(c, id);
			if (!c.Boolean(base + "_mapped")) return value;
			// Import projects a stored source endpoint array into rangeId, omitting the base slot.
			// A mapped flag alone leaves a stored scalar unchanged in NodeValue.getValue.
			const bool explicitScalar = authored(id), projectedRange = authored(rangeId) && !explicitScalar;
			const auto range = explicitScalar ? Vector2{value, value} : c.Vec2(rangeId);
			if (palette) return projectedRange ? range.X : value;
			const auto *map = c.Input(base + "_map");
			if (!map) return projectedRange ? range.X : value;
			auto texel = SampleNearest(*map, u, v);
			if (map->Format == SurfaceFormat::R8Unorm || map->Format == SurfaceFormat::R16Float ||
				map->Format == SurfaceFormat::R32Float)
				texel = {texel[0], texel[0], texel[0], 1};
			double mean = 0;
			for (size_t i = 0; i < 3; ++i)
				mean += Quantize(texel[i] * texel[3]) / 255.;
			return range.X + (range.Y - range.X) * mean / 3.;
		}

		bool ColourAdjust(NodeContext &c) {
			const int64_t type = c.Integer("input_type");
			const auto *source = c.Input("surface_in");
			if (!c.Boolean("active", true)) {
				bool failed = false;
				if (CopyWhenInactive(c, failed)) {
					if (failed) return false;
					return c.SetOutputDiagnostic(
						"color_out",
						Status::UnsupportedExecution,
						"Inactive Color Adjust retains an uncaptured palette output"
					);
				}
			}
			const auto blend = FilterColour(c.Get<Colour>("blend", Colour{255, 255, 255, 255}));
			const auto controls = [&](double u, double v, bool palette) {
				return std::array<double, 8>{
					ColourFilterControl(c, "brightness", u, v, palette),
					ColourFilterControl(c, "contrast", u, v, palette),
					ColourFilterControl(c, "exposure", u, v, palette),
					ColourFilterControl(c, "hue", u, v, palette),
					ColourFilterControl(c, "saturation", u, v, palette),
					ColourFilterControl(c, "value", u, v, palette),
					ColourFilterControl(c, "blend_amount", u, v, palette),
					ColourFilterControl(c, "alpha", u, v, palette)
				};
			};
			if (type == 1) {
				ColourFilterPalette colours;
				if (!ReadColourFilterPalette(c, "color", colours, Limits::MaximumArrayElements)) return false;
				const auto values = controls(0, 0, true);
				if (c.FailureCode != Status::Ok) return false;
				for (double value : values)
					if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
						return c.Fail(
							Status::InvalidValue,
							"Color Adjust controls must fit finite shader floats",
							"color"
						);
				if (!c.ReserveOutput(colours.Count * sizeof(ElementValue), "color_out")) return false;
				ArrayValue result{ValueType::Colour, {}};
				result.Elements.reserve(colours.Count);
				for (size_t i = 0; i < colours.Count; ++i) {
					const auto original = *colours.At(i);
					const auto rgba = FilterColour(original);
					Rgb3 intermediate{};
					for (size_t k = 0; k < 3; ++k)
						intermediate[k] =
							ColourFilterByte(
								std::clamp(
									(.5 + values[1] * 2 * (rgba[k] - .5) + values[0]) * values[2], 0., 1.
								) *
								255
							) /
							255.;
					// Native packed-HSV profile makes byte getters explicit; builtin rounding is not GPU
					// parity evidence.
					auto hsv = ShaderRgbToHsv(intermediate);
					for (auto &component : hsv)
						component = ColourFilterByte(component * 255) / 255.;
					hsv[0] = ShaderFract(hsv[0] + values[3]);
					hsv[2] = std::clamp((hsv[2] + values[5]) * (1 + values[4] * hsv[1] * .5), 0., 1.);
					hsv[1] = std::clamp(hsv[1] * (values[4] + 1), 0., 1.);
					const auto rgb = ShaderHsvToRgb(hsv);
					Colour colour{};
					colour.Red = ColourFilterByte(
						ColourFilterByte(rgb[0] * 255) +
						(double(blend[0] * 255) - ColourFilterByte(rgb[0] * 255)) * values[6]
					);
					colour.Green = ColourFilterByte(
						ColourFilterByte(rgb[1] * 255) +
						(double(blend[1] * 255) - ColourFilterByte(rgb[1] * 255)) * values[6]
					);
					colour.Blue = ColourFilterByte(
						ColourFilterByte(rgb[2] * 255) +
						(double(blend[2] * 255) - ColourFilterByte(rgb[2] * 255)) * values[6]
					);
					colour.Alpha =
						ColourFilterByte(original.Alpha + (blend[3] * 255 - original.Alpha) * values[6]);
					result.Elements.emplace_back(colour);
				}
				c.SetValue("color_out", std::move(result));
			} else
				c.SetValue("color_out", ArrayValue{ValueType::Colour, {}});
			if (!source)
				return c.SetOutputDiagnostic(
					"surface_out",
					Status::UnsupportedExecution,
					"Color Adjust retains an absent source surface output"
				);
			const auto format = ResolveProcessorSurfaceFormat(c, source);
			if (!format) return false;
			auto *output = c.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			const auto *mask = c.Input("mask");
			Image modified;
			const double feather = c.Scalar("mask_feather");
			if (!std::isfinite(feather) || feather > std::numeric_limits<int>::max())
				return c.Fail(
					Status::LimitExceeded, "Color Adjust mask feather exceeds bounded radius", "mask_feather"
				);
			const auto maskLayout =
				mask ? CheckedSurfaceLayout(
						   mask->Width, mask->Height, SurfaceFormat::RGBA8Unorm, Limits::MaximumOutputBytes
					   )
					 : std::optional<SurfaceLayout>{};
			const bool modify = mask && (c.Boolean("invert_mask") || feather != 0);
			if (modify && !maskLayout)
				return c.Fail(Status::LimitExceeded, "Color Adjust mask exceeds bounded surface", "mask");
			auto charge = c.ReserveWorkspace(
				modify ? 3 * maskLayout->Bytes +
							 sizeof(double) * uint64_t(std::max(1., std::round(std::max(0., feather))))
					   : 0,
				"mask_feather"
			);
			if (!charge) return false;
			if (modify) {
				modified = Image{mask->Width, mask->Height, std::vector<uint8_t>(maskLayout->Bytes), 0};
				for (uint32_t y = 0; y < mask->Height; ++y)
					for (uint32_t x = 0; x < mask->Width; ++x) {
						const auto sample = ReadPixel(*mask, x, y);
						double amount = c.Boolean("mask_alpha_only")
											? sample[3]
											: (sample[0] + sample[1] + sample[2]) / 3.;
						if (c.Boolean("invert_mask")) amount = 1 - amount;
						const Rgba value = c.Boolean("mask_alpha_only")
											   ? Rgba{1, 1, 1, amount}
											   : Rgba{amount, amount, amount, sample[3]};
						if (!WritePixel(modified, x, y, value))
							return c.Fail(
								Status::InvalidValue, "Color Adjust mask modifier is nonfinite", "mask"
							);
					}
				if (feather > 0) modified = FeatherMask(modified, feather);
				mask = &modified;
			}
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x) {
					const double u = (x + .5) / source->Width, v = (y + .5) / source->Height;
					const auto values = controls(u, v, false);
					if (c.FailureCode != Status::Ok) return false;
					for (double value : values)
						if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
							return c.Fail(
								Status::InvalidValue,
								"Color Adjust controls must fit finite shader floats",
								"surface_out"
							);
					const auto original = ReadPixel(*source, x, y);
					Rgb3 rgb{};
					for (size_t i = 0; i < 3; ++i) {
						const double contrast = std::clamp(.5 + values[1] * 2 * (original[i] - .5), 0., 1.);
						rgb[i] = std::clamp(std::clamp(contrast + values[0], 0., 1.) * values[2], 0., 1.);
					}
					auto hsv = ShaderRgbToHsv(rgb);
					hsv[0] = ShaderFract(ShaderFract(hsv[0] + values[3]) + 1);
					hsv[2] = std::clamp((hsv[2] + values[5]) * (1 + values[4] * hsv[1] * .5), 0., 1.);
					hsv[1] = std::clamp(hsv[1] * (values[4] + 1), 0., 1.);
					rgb = ColourFilterBlend(
						ShaderHsvToRgb(hsv),
						{blend[0], blend[1], blend[2]},
						values[6] * blend[3],
						c.Integer("blend_mode")
					);
					Rgba result{rgb[0], rgb[1], rgb[2], original[3] * values[7]};
					if (mask) {
						auto m = SampleNearest(*mask, u, v);

						for (size_t i = 0; i < 3; ++i) {
							const double amount = m[i] * m[3];
							result[i] = rgb[i] * amount + original[i] * (1 - amount);
						}
						result[3] = original[3] * (1 + (values[7] - 1) * m[0] * m[3]);
					}
					if (!WritePixel(*output, x, y, result))
						return c.Fail(
							Status::InvalidValue, "Color Adjust sample is nonfinite", "surface_out"
						);
				}
			return c.FailureCode == Status::Ok;
		}

		bool ColourSeparate(NodeContext &c) {
			const auto *source = c.Input("surface_in");
			if (!source) {
				if (!c.ReserveOutput(std::string{"surface_out"}.capacity(), "surface_out")) return false;
				c.OutputImageArrays.emplace_back("surface_out", ImageArray{});
				return true;
			}
			std::array<Colour, 128> extracted{};
			size_t amount = 0;
			ColourFilterPalette palette;
			const bool all = c.Boolean("all_colors", true), match = !all && c.Boolean("match_all", true);
			if (all) {
				// Source extractAll reads packed buffer_u32. Other surface formats require that host
				// observation.
				if (source->Format != SurfaceFormat::RGBA8Unorm) return ReplayRecordedHostOutputs(c);
				for (size_t i = 0; i < source->Pixels.size(); i += 4) {
					Colour col{
						source->Pixels[i], source->Pixels[i + 1], source->Pixels[i + 2], source->Pixels[i + 3]
					};
					if (!col.Alpha) continue;
					if (std::find(extracted.begin(), extracted.begin() + amount, col) !=
						extracted.begin() + amount)
						continue;
					if (amount == extracted.size()) return ReplayRecordedHostOutputs(c);
					extracted[amount++] = col;
				}
				// Native packed-HSV ordering profile mirrors source __sortHue's descending weighted key.
				const auto key = [](Colour col) {
					const auto hsv = FilterHsv(FilterColour(col));
					return uint32_t(ColourFilterByte(hsv[0] * 255)) * 65536 +
						   uint32_t(ColourFilterByte(hsv[1] * 255)) * 256 + ColourFilterByte(hsv[2] * 255);
				};
				// Fixed-capacity insertion sort preserves scan order for equal native keys without heap work.
				for (size_t i = 1; i < amount; ++i) {
					const Colour colour = extracted[i];
					size_t position = i;
					while (position && key(colour) > key(extracted[position - 1])) {
						extracted[position] = extracted[position - 1];
						--position;
					}
					extracted[position] = colour;
				}
			} else {
				if (!ReadColourFilterPalette(c, "colors", palette, 128)) return false;
				amount = palette.Count;
			}
			const auto colour = [&](size_t i) { return all ? extracted[i] : *palette.At(i); };
			if (source->Width > c.Request.MaximumImageDimension ||
				source->Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Separate Color dimensions exceed request bounds", "surface_out"
				);
			const auto layout = CheckedSurfaceLayout(
				source->Width, source->Height, SurfaceFormat::RGBA8Unorm, Limits::MaximumOutputBytes
			);
			if (!layout)
				return c.Fail(
					Status::LimitExceeded, "Separate Color dimensions exceed image bounds", "surface_out"
				);
			const uint64_t bytes =
				uint64_t(amount) * (layout->Bytes + sizeof(Image) + sizeof(ImageArrayItem));
			if (!c.ReserveOutput(bytes + std::string{"surface_out"}.capacity(), "surface_out")) return false;
			ImageArray outputs;
			outputs.Images.reserve(amount);
			outputs.Items.reserve(amount);
			for (size_t index = 0; index < amount; ++index) {
				auto *owned =
					c.NewImage("surface_out", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
				if (!owned) return false;
				Image &image = *owned;
				const auto target = FilterColour(colour(index));
				for (uint32_t y = 0; y < source->Height; ++y)
					for (uint32_t x = 0; x < source->Width; ++x) {
						const auto input = ReadPixel(*source, x, y);
						bool keep = input == target;
						if (match) {
							const auto lab = FilterLab({input[0], input[1], input[2]});
							double nearest = 9999.;
							size_t selected = 0;
							for (size_t k = 0; k < amount; ++k) {
								const auto rgba = FilterColour(colour(k));
								const auto d = FilterDistance(lab, FilterLab({rgba[0], rgba[1], rgba[2]}));
								if (d <= nearest) {
									nearest = d;
									selected = k;
								}
							}
							keep = colour(selected) == colour(index);
						}
						if (!WritePixel(image, x, y, keep ? input : Rgba{}))
							return c.Fail(
								Status::InvalidValue, "Separate Color sample is nonfinite", "surface_out"
							);
					}
				image.Hash = SurfaceHash(image);
				outputs.Items.push_back({outputs.Images.size()});
				outputs.Images.push_back(std::move(image));
				c.OutputImages.clear();
			}
			c.OutputImageArrays.emplace_back("surface_out", std::move(outputs));
			return c.FailureCode == Status::Ok;
		}

	}
	std::span<const ExecutorEntry> SourceColourFilterExecutors() {
		static const ExecutorEntry entries[] = {
			{"pc.color_adjust", ColourAdjust, true},
			{"pc.color_replace", ColourReplace, true},
			{"pc.color_separate", ColourSeparate, true},
			{"pc.colors_replace", ColoursReplace, true}
		};
		return entries;
	}

}
