#include "../SourceBuiltinRandomContext.hpp"
#include "../Utf8TextOps.hpp"
#include "Families.hpp"
#include "SourceInterpret.hpp"

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t PALETTE_COMPARISON_LIMIT = 1u << 24;
		struct SourcePaletteInput {
			const ArrayValue *Array = nullptr;
			size_t Count = 0;
			const ElementValue &Leaf(size_t index) const {
				return Array->Items.empty() ? Array->Elements[index]
											: std::get<ElementValue>(Array->Items[index].Data);
			}
			Colour At(size_t index) const {
				return *std::visit([](const auto &raw) { return InterpretPackedColour(raw); }, Leaf(index));
			}
		};
		bool SourceReadPalette(NodeContext &c, std::string_view port, SourcePaletteInput &input) {
			const auto *value = c.Find(port);
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!array || !array->Nested.empty())
				return c.Fail(
					Status::UnsupportedExecution, "Palette input must be a flat source array", port
				);
			input = {array, array->Items.empty() ? array->Elements.size() : array->Items.size()};
			if (input.Count > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Palette exceeds element bounds", port);
			for (size_t i = 0; i < input.Count; ++i) {
				const auto *leaf = array->Items.empty() ? &array->Elements[i]
														: std::get_if<ElementValue>(&array->Items[i].Data);
				if (!leaf || !std::visit([](const auto &raw) { return InterpretPackedColour(raw); }, *leaf))
					return c.Fail(
						Status::UnsupportedExecution,
						"Palette colour has no resolved packed representation",
						port
					);
			}
			return true;
		}
		uint8_t SourcePaletteByte(double value) {
			value = std::clamp(value, 0., 255.);
			const double floor = std::floor(value), fraction = value - floor;
			return uint8_t(floor + (fraction > .5 || (fraction == .5 && std::fmod(floor, 2) != 0)));
		}
		Rgb3 SourcePaletteRgb(Colour colour) {
			return {colour.Red / 255., colour.Green / 255., colour.Blue / 255.};
		}
		// These are the fractional Color_RGBtoHSV getters in the pinned official HTML5 runtime.
		Rgb3 SourcePaletteHsvGetters(Colour colour) {
			const auto rgb = SourcePaletteRgb(colour);
			const double minimum = std::min({rgb[0], rgb[1], rgb[2]}),
						 value = std::max({rgb[0], rgb[1], rgb[2]});
			const double delta = value - minimum, saturation = value == 0 ? 0 : delta / value;
			double hue = 0;
			if (saturation != 0) {
				if (rgb[0] == value)
					hue = 60 * (rgb[1] - rgb[2]) / delta;
				else if (rgb[1] == value)
					hue = 120 + 60 * (rgb[2] - rgb[0]) / delta;
				else
					hue = 240 + 60 * (rgb[0] - rgb[1]) / delta;
				if (hue < 0) hue += 360;
			}
			return {
				std::clamp(hue * 255 / 360., 0., 255.),
				std::clamp(saturation * 255, 0., 255.),
				std::clamp(value * 255, 0., 255.)
			};
		}
		Rgb3 SourcePaletteHsv(Colour colour) {
			auto hsv = SourcePaletteHsvGetters(colour);
			for (auto &component : hsv)
				component /= 255;
			return hsv;
		}
		Colour SourcePaletteHsvColour(Rgb3 hsv) {
			double hue = (hsv[0] * 255) * 360 / 255;
			if (hue == 360) hue = 0;
			const double saturation = (hsv[1] * 255) / 255, value = (hsv[2] * 255) / 255;
			Rgb3 rgb{value, value, value};
			if (saturation != 0) {
				hue /= 60;
				const int sector = int(std::floor(hue));
				const double fraction = hue - sector, p = value * (1 - saturation),
							 q = value * (1 - saturation * fraction),
							 t = value * (1 - saturation * (1 - fraction));
				switch (sector) {
				case 0:
					rgb = {value, t, p};
					break;
				case 1:
					rgb = {q, value, p};
					break;
				case 2:
					rgb = {p, value, t};
					break;
				case 3:
					rgb = {p, q, value};
					break;
				case 4:
					rgb = {t, p, value};
					break;
				default:
					rgb = {value, p, q};
					break;
				}
			}
			// make_color_hsv uses half-up channel rounding, unlike the project's make_color_rgba.
			return {
				uint8_t(std::clamp(std::floor(rgb[0] * 255 + .5), 0., 255.)),
				uint8_t(std::clamp(std::floor(rgb[1] * 255 + .5), 0., 255.)),
				uint8_t(std::clamp(std::floor(rgb[2] * 255 + .5), 0., 255.)),
				255
			};
		}
		double SourcePaletteDistance(const Rgb3 &a, const Rgb3 &b) {
			double sum = 0;
			for (size_t i = 0; i < 3; ++i)
				sum += (a[i] - b[i]) * (a[i] - b[i]);
			return std::sqrt(sum);
		}
		bool SourceReservePalette(
			NodeContext &c,
			size_t count,
			std::string_view port,
			ArrayValue &output,
			const SourcePaletteInput *source = nullptr,
			bool general = false
		) {
			if (count > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Palette output exceeds bounded elements", port);
			if (!c.ReserveOutput(
					count * (general || (source && !source->Array->Items.empty())
								 ? sizeof(decltype(output.Items)::value_type)
								 : sizeof(ElementValue)) +
						std::max(port.size(), std::string{}.capacity()),
					port
				))
				return false;
			output.ElementType = general  ? ValueType::Any
								 : source ? source->Array->ElementType
										  : ValueType::Colour;
			if (general || (source && !source->Array->Items.empty()))
				output.Items.reserve(count);
			else
				output.Elements.reserve(count);
			return true;
		}
		void SourceAppendPalette(ArrayValue &output, const ElementValue &leaf) {
			if (output.Items.capacity())
				output.Items.emplace_back(leaf);
			else
				output.Elements.push_back(leaf);
		}
		bool SourcePublishPalette(
			NodeContext &c, std::string_view port, const SourcePaletteInput &input, size_t begin, size_t end
		) {
			ArrayValue output;
			if (!SourceReservePalette(c, end - begin, port, output, &input)) return false;
			for (size_t i = begin; i < end; ++i)
				SourceAppendPalette(output, input.Leaf(i));
			c.SetValue(port, std::move(output));
			return c.FailureCode == Status::Ok;
		}
		bool SourceTrimPalette(NodeContext &c) {
			SourcePaletteInput input;
			if (!SourceReadPalette(c, "palette", input)) return false;
			const auto range = c.Vec2("trim_range", {0, 1});
			if (!std::isfinite(range.X) || !std::isfinite(range.Y))
				return c.Fail(Status::InvalidValue, "Palette trim endpoints must be finite", "trim_range");
			const size_t begin =
				size_t(std::floor(std::clamp(std::min(range.X, range.Y), 0., 1.) * input.Count));
			const size_t end =
				size_t(std::floor(std::clamp(std::max(range.X, range.Y), 0., 1.) * input.Count));
			return SourcePublishPalette(c, "palette", input, begin, end);
		}
		bool SourceReplacePalette(NodeContext &c) {
			SourcePaletteInput input, from, to;
			if (!SourceReadPalette(c, "palette_in", input) || !SourceReadPalette(c, "palette_from", from) ||
				!SourceReadPalette(c, "palette_to", to))
				return false;
			if (input.Count &&
				from.Count > PALETTE_COMPARISON_LIMIT / std::max<size_t>(1, c.ProcessorCount) / input.Count)
				return c.Fail(
					Status::LimitExceeded, "Palette replacement exceeds comparison budget", "palette_from"
				);
			const double threshold = c.Scalar("threshold", .1);
			if (!std::isfinite(threshold))
				return c.Fail(Status::InvalidValue, "Palette threshold must be finite", "threshold");
			ArrayValue output;
			if (!SourceReservePalette(
					c,
					input.Count,
					"surface_out",
					output,
					&input,
					input.Array->ElementType != to.Array->ElementType || !to.Array->Items.empty()
				))
				return false;
			for (size_t i = 0; i < input.Count; ++i) {
				const auto original = input.At(i);
				double nearest = 999;
				size_t index = from.Count;
				for (size_t j = 0; j < from.Count; ++j) {
					const double distance =
						SourcePaletteDistance(SourcePaletteRgb(original), SourcePaletteRgb(from.At(j)));
					if (distance <= threshold && distance < nearest) {
						nearest = distance;
						index = j;
					}
				}
				// The source passes a fourth overflow argument to the three-argument fast helper. It is
				// ignored.
				SourceAppendPalette(
					output, index < from.Count && index < to.Count ? to.Leaf(index) : input.Leaf(i)
				);
			}
			c.SetValue("surface_out", std::move(output));
			return c.FailureCode == Status::Ok;
		}
		struct SourcePaletteSortItem {
			double Key;
			size_t Index;
		};
		bool SourceSortPalette(NodeContext &c) {
			SourcePaletteInput input;
			if (!SourceReadPalette(c, "palette_in", input)) return false;
			const int64_t order = c.Integer("order");
			const auto *raw = c.Find("sort_order");
			const auto *text = raw ? std::get_if<std::string>(raw) : nullptr;
			std::string_view custom = text ? std::string_view(*text) : std::string_view{"RGB"};
			size_t length = 0;
			if (order == 10 && CountText(custom, length) == TextOpStatus::InvalidUtf8)
				return c.Fail(Status::InvalidValue, "Custom palette key must be valid UTF-8", "sort_order");
			if (order == 10 && (CountText(custom, length) != TextOpStatus::Ok || length > 127))
				return c.Fail(
					Status::LimitExceeded,
					"Custom palette key must be valid UTF-8 with at most 127 characters",
					"sort_order"
				);
			auto charge = c.ReserveWorkspace(input.Count * sizeof(SourcePaletteSortItem), "palette_in");
			if (!charge) return false;
			std::vector<SourcePaletteSortItem> rows;
			rows.reserve(input.Count);
			const auto key = [&](Colour colour) {
				const auto rgb = SourcePaletteRgb(colour), hsv = SourcePaletteHsvGetters(colour);
				switch (order) {
				case 0:
					return .299 * colour.Red + .587 * colour.Green + .114 * colour.Blue;
				case 2:
					return hsv[0] * 65536 + hsv[1] * 256 + hsv[2];
				case 3:
					return hsv[0] * 256 + hsv[1] * 65536 + hsv[2];
				case 4:
					return hsv[0] * 256 + hsv[1] + hsv[2] * 65536;
				case 6:
					return double(uint32_t(colour.Red) * 65536 + uint32_t(colour.Green) * 256 + colour.Blue);
				case 7:
					return double(colour.Red + uint32_t(colour.Green) * 65536 + uint32_t(colour.Blue) * 256);
				case 8:
					return double(uint32_t(colour.Red) * 256 + colour.Green + uint32_t(colour.Blue) * 65536);
				case 10: {
					double result = 0, weight = length ? std::pow(256., double(length - 1)) : 0;
					for (size_t offset = 0; offset < custom.size();) {
						const auto character = uint8_t(custom[offset]);
						NextUtf8(custom, offset);
						const auto lower =
							character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
						double component = 0;
						switch (lower) {
						case 'r':
							component = colour.Red;
							break;
						case 'g':
							component = colour.Green;
							break;
						case 'b':
							component = colour.Blue;
							break;
						case 'h':
							component = hsv[0];
							break;
						case 's':
							component = hsv[1];
							break;
						case 'v':
							component = hsv[2];
							break;
						// colorBrightness(false) deliberately uses .224 Blue, unlike __sortBright's .114.
						case 'l':
							component = (.299 * rgb[0] + .587 * rgb[1] + .224 * rgb[2]) * 255;
							break;
						default:
							break;
						}
						result += (character <= 'Z' ? component : 256 - component) * weight;
						weight /= 256;
					}
					return result;
				}
				default:
					return 0.;
				}
			};
			for (size_t i = 0; i < input.Count; ++i) {
				const auto colour = input.At(i);
				rows.push_back({key(colour), i});
			}
			// Stable original order is the bounded native equal-key profile, not measured GameMaker sort
			// parity.
			std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
				return a.Key > b.Key || (a.Key == b.Key && a.Index < b.Index);
			});
			if (c.Boolean("reverse")) std::reverse(rows.begin(), rows.end());
			ArrayValue output;
			if (!SourceReservePalette(c, input.Count, "sorted_palette", output, &input)) return false;
			for (const auto &row : rows)
				SourceAppendPalette(output, input.Leaf(row.Index));
			c.SetValue("sorted_palette", std::move(output));
			return c.FailureCode == Status::Ok;
		}
		struct SourcePaletteCluster {
			Rgb3 Value{};
			size_t Count = 0;
		};
		bool SourceShrinkPalette(NodeContext &c) {
			SourcePaletteInput input;
			if (!SourceReadPalette(c, "palette_in", input)) return false;
			const int64_t requested = c.Integer("amount", 4);
			if (requested >= 0 && uint64_t(requested) >= input.Count)
				return SourcePublishPalette(c, "palette", input, 0, input.Count);
			const size_t count = size_t(std::max<int64_t>(1, requested));
			if (count > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Palette clusters exceed bounded elements", "amount");
			const int64_t algorithm = c.Integer("algorithm", 1), space = c.Integer("color_space"),
						  sample = c.Integer("sample_type"), shift = c.Integer("shift");
			if (algorithm != 0 && algorithm != 1)
				return SourcePublishPalette(c, "palette", input, 0, input.Count);
			if (!input.Count || ((algorithm == 0 || (sample == 0 && shift == 0)) && count == 1)) {
				if (c.ProcessorCount > 1)
					return c.Fail(
						Status::UnsupportedExecution,
						"Singular palette arithmetic needs a per-row host receipt unavailable in this "
						"capture format",
						"amount"
					);
				return ReplayRecordedHostOutputs(c);
			}
			if (space < 0 || space > 1)
				return c.Fail(
					Status::UnsupportedExecution, "Palette colour space has no source branch", "color_space"
				);
			if (algorithm == 1 && sample != 0 && sample != 1)
				return c.Fail(
					Status::UnsupportedExecution, "Palette sample type has no source branch", "sample_type"
				);
			if (algorithm == 1 && input.Count &&
				count > PALETTE_COMPARISON_LIMIT / std::max<size_t>(1, c.ProcessorCount) / 11 / input.Count)
				return c.Fail(
					Status::LimitExceeded, "Palette clustering exceeds comparison budget", "amount"
				);
			Rgb3 minimum{1, 1, 1}, maximum{};
			auto charge = c.ReserveWorkspace(
				input.Count * sizeof(Rgb3) + count * (2 * sizeof(SourcePaletteCluster) + sizeof(size_t)),
				"palette_in"
			);
			if (!charge) return false;
			std::vector<Rgb3> points;
			points.reserve(input.Count);
			for (size_t i = 0; i < input.Count; ++i) {
				const auto point = space ? SourcePaletteHsv(input.At(i)) : SourcePaletteRgb(input.At(i));
				points.push_back(point);
				for (size_t k = 0; k < 3; ++k) {
					minimum[k] = std::min(minimum[k], point[k]);
					maximum[k] = std::max(maximum[k], point[k]);
				}
			}
			ArrayValue output;
			if (!SourceReservePalette(c, count, "palette", output, algorithm == 1 ? &input : nullptr))
				return false;
			if (algorithm == 0) {
				for (size_t i = 0; i < count; ++i) {
					Rgb3 value{};
					for (size_t k = 0; k < 3; ++k)
						value[k] = minimum[k] + (maximum[k] - minimum[k]) * double(i) / double(count - 1);
					if (space) {
						output.Elements.emplace_back(SourcePaletteHsvColour(value));
						continue;
					}
					output.Elements.emplace_back(
						Colour{
							SourcePaletteByte(value[0] * 255),
							SourcePaletteByte(value[1] * 255),
							SourcePaletteByte(value[2] * 255),
							255
						}
					);
				}
			} else {
				const SourceBuiltinRandomCapture *capture = nullptr;
				if (sample == 1 && !FindSourceBuiltinRandomCapture(c, capture)) return false;
				if (capture && capture->Draws.size() != count)
					return c.Fail(
						Status::InvalidValue,
						"Palette random cluster observation count differs from source calls",
						"seed"
					);
				std::vector<SourcePaletteCluster> centres(count), sums(count);
				for (size_t i = 0; i < count; ++i) {
					const double offset = double(i) / count + double(shift);
					double fraction = shift ? offset - std::trunc(offset) : double(i) / double(count - 1);
					if (capture) {
						const auto &draw = capture->Draws[i];
						if (draw.Operation != SourceBuiltinRandomOperation::Random || draw.Lower != 0 ||
							draw.Upper != 1 || !std::isfinite(draw.Result) || draw.Result < 0 ||
							draw.Result >= 1)
							return c.Fail(
								Status::InvalidValue,
								"Palette cluster random observation differs from random(1)",
								"seed"
							);
						fraction = draw.Result;
					}
					for (size_t k = 0; k < 3; ++k)
						centres[i].Value[k] = minimum[k] + (maximum[k] - minimum[k]) * fraction;
				}
				for (size_t iteration = 0; iteration < 10; ++iteration) {
					std::fill(sums.begin(), sums.end(), SourcePaletteCluster{});
					for (size_t i = 0; i < points.size(); ++i) {
						double nearest = 999;
						size_t selected = 0;
						for (size_t j = 0; j < count; ++j) {
							const double distance = SourcePaletteDistance(points[i], centres[j].Value);
							if (distance < nearest) {
								nearest = distance;
								selected = j;
							}
						}
						auto &sum = sums[selected];
						++sum.Count;
						for (size_t k = 0; k < 3; ++k)
							sum.Value[k] += points[i][k];
					}
					for (size_t j = 0; j < count; ++j)
						for (size_t k = 0; k < 3; ++k)
							centres[j].Value[k] = sums[j].Count ? sums[j].Value[k] / sums[j].Count : 0;
				}
				std::vector<size_t> indices;
				indices.reserve(count);
				for (const auto &centre : centres) {
					double nearest = 999;
					size_t selected = 0;
					for (size_t i = 0; i < points.size(); ++i) {
						const double distance = SourcePaletteDistance(centre.Value, points[i]);
						if (distance < nearest) {
							nearest = distance;
							selected = i;
						}
					}
					indices.push_back(selected);
				}
				std::sort(indices.begin(), indices.end());
				indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
				for (size_t i : indices)
					SourceAppendPalette(output, input.Leaf(i));
			}
			c.SetValue("palette", std::move(output));
			return c.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourcePaletteExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.palette", SourceTrimPalette, true},
			{"pc.palette_sort", SourceSortPalette, true},
			{"pc.palette_replace", SourceReplacePalette, true},
			{"pc.palette_shrink", SourceShrinkPalette, true}
		};
		return entries;
	}
}
