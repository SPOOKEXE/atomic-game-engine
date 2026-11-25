#include "Properties.hpp"

#include <engine/ang/Value.hpp>
#include <engine/imagegraph/Reference.hpp>
#include <engine/imagegraph/Schema.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

// Every node kind describes its properties once, in Describe. The visitors below turn
// that one description into strict JSON reading and writing, validation, bindings,
// host editing specs and the GPU cache fingerprint.
namespace engine::imagegraph {
	namespace {
		using nlohmann::json;
		using Names = std::span<const std::string_view>;
		constexpr double UNBOUNDED = std::numeric_limits<double>::max();

		constexpr std::array<std::string_view, 2> INTERPRETATIONS{"colour", "data"};
		constexpr std::array<std::string_view, 2> FILTERS{"nearest", "bilinear"};
		constexpr std::array<std::string_view, 3> LOOKUP_WRAPS{"clamp", "repeat", "transparent"};
		constexpr std::array<std::string_view, 8> BLEND_MODES{
			"normal", "multiply", "screen", "overlay", "add", "subtract", "soft_light", "difference"
		};
		constexpr std::array<std::string_view, 3> NOISE_TYPES{"value", "gradient", "gabor"};
		constexpr std::array<std::string_view, 5> VORONOI_FEATURES{"f1", "f2", "f2_f1", "edge", "cell"};
		constexpr std::array<std::string_view, 2> GRADIENT_TYPES{"linear", "radial"};
		constexpr std::array<std::string_view, 2> WAVE_TYPES{"bands", "rings"};
		constexpr std::array<std::string_view, 3> WAVE_PROFILES{"sine", "saw", "triangle"};
		constexpr std::array<std::string_view, 5> CHANNELS{"red", "green", "blue", "alpha", "luminance"};
		constexpr std::array<std::string_view, 3> INTERPOLATIONS{"linear", "constant", "smooth"};
		constexpr std::array<std::string_view, 7> MATH_OPERATIONS{
			"add", "subtract", "multiply", "divide", "minimum", "maximum", "power"
		};
		constexpr std::string_view CHANNEL_LETTERS = "rgba";

		bool PortableText(std::string_view text) {
			return text.size() <= 4096 &&
				   std::none_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 32; });
		}
		bool ComponentsValid(std::string_view text) {
			if (text.empty() || text.size() > 3) return false;
			for (size_t index = 0; index < text.size(); ++index) {
				if (std::string_view("xyzw").find(text[index]) == std::string_view::npos) return false;
				if (text.substr(0, index).find(text[index]) != std::string_view::npos) return false;
			}
			return true;
		}

		// Text spellings shared by GetProperty, SetProperty and JSON.
		std::string FormatNumber(double value) {
			std::array<char, 32> buffer{};
			const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			return std::string(buffer.data(), result.ptr);
		}
		bool ParseNumber(std::string_view text, double &out) {
			const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
			return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(out);
		}
		std::vector<std::string_view> Words(std::string_view text) {
			std::vector<std::string_view> words;
			size_t begin = 0;
			while (begin < text.size()) {
				while (begin < text.size() && text[begin] == ' ')
					++begin;
				size_t end = begin;
				while (end < text.size() && text[end] != ' ')
					++end;
				if (end > begin) words.push_back(text.substr(begin, end - begin));
				begin = end;
			}
			return words;
		}
		std::string FormatMask(ChannelSet mask) {
			std::string text;
			for (size_t channel = 0; channel < 4; ++channel)
				if (mask & (1u << channel)) text += CHANNEL_LETTERS[channel];
			return text;
		}
		bool ParseMask(std::string_view text, ChannelSet &out) {
			ChannelSet mask = 0;
			for (char letter : text) {
				const auto channel = CHANNEL_LETTERS.find(letter);
				if (channel == std::string_view::npos || (mask & (1u << channel))) return false;
				mask = static_cast<ChannelSet>(mask | (1u << channel));
			}
			if (mask == 0) return false;
			out = mask;
			return true;
		}
		std::string FormatPick(const ChannelPick &pick) {
			if (pick.Input == -1) return "zero";
			if (pick.Input == -2) return "one";
			return std::to_string(pick.Input) + "." + CHANNEL_LETTERS[pick.Channel & 3];
		}
		bool ParsePick(std::string_view text, ChannelPick &out) {
			if (text == "zero" || text == "one") {
				out = {static_cast<int8_t>(text == "zero" ? -1 : -2), 0};
				return true;
			}
			if (text.size() != 3 || text[1] != '.' || text[0] < '0' || text[0] > '3') return false;
			const auto channel = CHANNEL_LETTERS.find(text[2]);
			if (channel == std::string_view::npos) return false;
			out = {static_cast<int8_t>(text[0] - '0'), static_cast<uint8_t>(channel)};
			return true;
		}
		std::string FormatPicks(const std::array<ChannelPick, 4> &picks) {
			std::string text;
			for (const auto &pick : picks)
				text += (text.empty() ? "" : " ") + FormatPick(pick);
			return text;
		}
		bool ParsePicks(std::string_view text, std::array<ChannelPick, 4> &out) {
			const auto words = Words(text);
			if (words.size() != 4) return false;
			std::array<ChannelPick, 4> picks{};
			for (size_t index = 0; index < 4; ++index)
				if (!ParsePick(words[index], picks[index])) return false;
			out = picks;
			return true;
		}
		std::string FormatColourHex(const std::array<uint8_t, 4> &colour) {
			constexpr char HEX[] = "0123456789abcdef";
			std::string text;
			for (uint8_t channel : colour) {
				text += HEX[channel >> 4];
				text += HEX[channel & 15];
			}
			return text;
		}
		bool ParseColourHex(std::string_view text, std::array<uint8_t, 4> &out) {
			if (text.size() != 8) return false;
			std::array<uint8_t, 4> colour{};
			for (size_t channel = 0; channel < 4; ++channel) {
				unsigned value = 0;
				const auto result =
					std::from_chars(text.data() + channel * 2, text.data() + channel * 2 + 2, value, 16);
				if (result.ec != std::errc{} || result.ptr != text.data() + channel * 2 + 2) return false;
				colour[channel] = static_cast<uint8_t>(value);
			}
			out = colour;
			return true;
		}
		std::string FormatStops(const std::vector<RampStop> &stops) {
			std::string text;
			for (const auto &stop : stops)
				text += (text.empty() ? "" : " ") + FormatNumber(stop.Position) + ":" +
						FormatColourHex(stop.Colour);
			return text;
		}
		bool ParseStops(std::string_view text, std::vector<RampStop> &out) {
			const auto words = Words(text);
			if (words.empty() || words.size() > Limits::MaximumRampStops) return false;
			std::vector<RampStop> stops;
			for (const auto word : words) {
				const auto colon = word.find(':');
				RampStop stop;
				if (colon == std::string_view::npos || !ParseNumber(word.substr(0, colon), stop.Position) ||
					!ParseColourHex(word.substr(colon + 1), stop.Colour))
					return false;
				stops.push_back(stop);
			}
			out = std::move(stops);
			return true;
		}
		std::string FormatVector(const std::array<double, 4> &vector) {
			std::string text;
			for (double value : vector)
				text += (text.empty() ? "" : " ") + FormatNumber(value);
			return text;
		}
		bool ParseVector(std::string_view text, std::array<double, 4> &out) {
			const auto words = Words(text);
			if (words.size() != 4) return false;
			std::array<double, 4> vector{};
			for (size_t index = 0; index < 4; ++index)
				if (!ParseNumber(words[index], vector[index])) return false;
			out = vector;
			return true;
		}

		bool PropertyDimensions(uint32_t width, uint32_t height) {
			return width > 0 && height > 0 && width <= Limits::MaximumDimension &&
				   height <= Limits::MaximumDimension &&
				   uint64_t(width) * height * 4 <= Limits::MaximumImageBytes;
		}

		// One description per kind. The visitor sees every property in serialization order.
		template <class V> void Describe(Source &value, V &v) {
			v.Text("path", value.Path);
			if (v.Live()) v.Choice("interpretation", value.Interpretation, INTERPRETATIONS, false);
			v.Check(
				detail::SourceValid(value.Path),
				"source must use a bounded relative path without parent segments"
			);
		}
		template <class V> void Extent(uint32_t &width, uint32_t &height, V &v) {
			v.Dimension("width", width);
			v.Dimension("height", height);
			v.Check(PropertyDimensions(width, height), "output dimensions exceed image limits");
		}
		template <class V> void Describe(Solid &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Colour("colour", value.Colour);
		}
		template <class V> void Describe(Resize &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Choice("filter", value.Filter, FILTERS);
		}
		template <class V> void Describe(Crop &value, V &v) {
			v.Offset("x", value.X, INT32_MIN, INT32_MAX);
			v.Offset("y", value.Y, INT32_MIN, INT32_MAX);
			Extent(value.Width, value.Height, v);
		}
		template <class V> void Describe(Transform &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Number("translate_x", value.TranslateX, -UNBOUNDED, UNBOUNDED);
			v.Number("translate_y", value.TranslateY, -UNBOUNDED, UNBOUNDED);
			v.Number("scale_x", value.ScaleX, -UNBOUNDED, UNBOUNDED);
			v.Number("scale_y", value.ScaleY, -UNBOUNDED, UNBOUNDED);
			v.Number("degrees", value.Degrees, -UNBOUNDED, UNBOUNDED);
			v.Number("pivot_x", value.PivotX, -UNBOUNDED, UNBOUNDED);
			v.Number("pivot_y", value.PivotY, -UNBOUNDED, UNBOUNDED);
			v.Choice("filter", value.Filter, FILTERS);
			v.Check(value.ScaleX != 0 && value.ScaleY != 0, "transform scales must be nonzero");
		}
		template <class V> void Describe(Flip &value, V &v) {
			v.Flag("horizontal", value.Horizontal);
			v.Flag("vertical", value.Vertical);
		}
		template <class V> void Describe(Blend &value, V &v) {
			v.Number("opacity", value.Opacity, 0, 1);
			// Omitted when normal, so documents without modes keep their original encoding.
			v.Choice("mode", value.Mode, BLEND_MODES, true, true);
		}
		template <class V> void Describe(Noise &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Choice("type", value.Type, NOISE_TYPES);
			v.Number("scale", value.Scale, 1e-6, 1e6);
			v.Count("octaves", value.Octaves, 1, 8);
			v.Number("lacunarity", value.Lacunarity, 1, 16);
			v.Number("gain", value.Gain, 0, 1);
			v.Count("seed", value.Seed, 0, UINT32_MAX);
			// Present only for gabor, so other noise keeps its encoding.
			if (value.Type != NoiseType::Gabor) return;
			v.Number("degrees", value.Degrees, -1e6, 1e6);
			v.Number("frequency", value.Frequency, 0, 64);
			v.Number("bandwidth", value.Bandwidth, 0, 16);
			v.Count("impulses", value.Impulses, 1, 16);
		}
		template <class V> void Describe(Voronoi &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Number("scale", value.Scale, 1e-6, 1e6);
			v.Number("randomness", value.Randomness, 0, 1);
			v.Choice("feature", value.Feature, VORONOI_FEATURES);
			v.Count("seed", value.Seed, 0, UINT32_MAX);
		}
		template <class V> void Describe(Gradient &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Choice("type", value.Type, GRADIENT_TYPES);
			v.Number("start_x", value.StartX, -1e6, 1e6);
			v.Number("start_y", value.StartY, -1e6, 1e6);
			v.Number("end_x", value.EndX, -1e6, 1e6);
			v.Number("end_y", value.EndY, -1e6, 1e6);
			v.Check(
				value.StartX != value.EndX || value.StartY != value.EndY, "gradient start and end must differ"
			);
		}
		template <class V> void Describe(Wave &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Choice("type", value.Type, WAVE_TYPES);
			v.Choice("profile", value.Profile, WAVE_PROFILES);
			v.Number("scale", value.Scale, 1e-6, 1e6);
			v.Number("degrees", value.Degrees, -1e6, 1e6);
			v.Number("phase", value.Phase, -1e6, 1e6);
			v.Number("distortion", value.Distortion, -1e3, 1e3);
			v.Number("detail_scale", value.DetailScale, 1e-6, 1e6);
			v.Count("detail", value.Detail, 1, 8);
			v.Count("seed", value.Seed, 0, UINT32_MAX);
		}
		template <class V> void Describe(Ramp &value, V &v) {
			v.Choice("factor", value.Factor, CHANNELS);
			v.Choice("interpolation", value.Interpolation, INTERPOLATIONS);
			v.Stops("stops", value.Stops);
			bool ordered = !value.Stops.empty() && value.Stops.size() <= Limits::MaximumRampStops;
			for (size_t index = 0; ordered && index < value.Stops.size(); ++index) {
				const double position = value.Stops[index].Position;
				ordered = std::isfinite(position) && position >= 0 && position <= 1 &&
						  (index == 0 || value.Stops[index - 1].Position <= position);
			}
			v.Check(ordered, "ramp needs 1 to 16 ascending stops within [0,1]");
		}
		template <class V> void Describe(Levels &value, V &v) {
			v.Number("input_minimum", value.InputMinimum, -16, 16);
			v.Number("input_maximum", value.InputMaximum, -16, 16);
			v.Number("output_minimum", value.OutputMinimum, -16, 16);
			v.Number("output_maximum", value.OutputMaximum, -16, 16);
			v.Number("gamma", value.Gamma, 1e-3, 1e3);
			v.Flag("clamp", value.Clamp);
			v.Mask("channels", value.Channels);
			v.Check(value.InputMinimum != value.InputMaximum, "levels input range must not be empty");
		}
		template <class V> void Describe(Math &value, V &v) {
			v.Choice("operation", value.Operation, MATH_OPERATIONS);
			v.Vector("value", value.Value);
			v.Mask("channels", value.Channels);
			v.Check(
				std::all_of(
					value.Value.begin(),
					value.Value.end(),
					[](double number) { return std::isfinite(number) && std::abs(number) <= 1e6; }
				),
				"math constants must be finite and within 1e6"
			);
		}
		template <class V> void Describe(Channels &value, V &v) {
			v.Picks("picks", value.Picks);
			v.Check(
				std::all_of(
					value.Picks.begin(),
					value.Picks.end(),
					[](const ChannelPick &pick) {
						return pick.Input >= -2 && pick.Input < 4 && pick.Channel < 4;
					}
				),
				"channel picks must name input 0 to 3 and r, g, b or a"
			);
		}
		template <class V> void Describe(Invert &value, V &v) {
			v.Mask("channels", value.Channels);
		}
		template <class V> void Describe(Blur &value, V &v) {
			v.Number("radius", value.Radius, 0, Limits::MaximumBlurRadius);
		}
		template <class V> void Describe(NormalMap &value, V &v) {
			v.Number("strength", value.Strength, -1e3, 1e3);
			v.Choice("height", value.Height, CHANNELS);
			v.Flag("flip_y", value.FlipY);
		}
		template <class V> void Describe(Warp &value, V &v) {
			v.Number("strength", value.Strength, -4096, 4096);
			v.Choice("filter", value.Filter, FILTERS);
		}
		template <class V> void Describe(MeshAttribute &value, V &v) {
			Extent(value.Width, value.Height, v);
			v.Text("mesh", value.Mesh);
			v.Count("attribute", value.Attribute, 0, 3);
			v.Text("components", value.Components);
			v.Offset("tile_u", value.TileU, -1024, 1024);
			v.Offset("tile_v", value.TileV, -1024, 1024);
			v.Number("minimum", value.Minimum, -1e9, 1e9);
			v.Number("maximum", value.Maximum, -1e9, 1e9);
			v.Count("padding", value.Padding, 0, Limits::MaximumPadding);
			v.Check(!value.Mesh.empty() && PortableText(value.Mesh), "mesh reference is empty or invalid");
			v.Check(ComponentsValid(value.Components), "components must be one to three of xyzw");
			v.Check(value.Minimum != value.Maximum, "attribute range must not be empty");
		}
		template <class V> void Describe(Lookup &value, V &v) {
			v.Choice("filter", value.Filter, FILTERS);
			v.Choice("wrap", value.Wrap, LOOKUP_WRAPS);
			v.Number("scale_x", value.ScaleX, -1e4, 1e4);
			v.Number("scale_y", value.ScaleY, -1e4, 1e4);
			v.Number("offset_x", value.OffsetX, -1e4, 1e4);
			v.Number("offset_y", value.OffsetY, -1e4, 1e4);
		}
		template <class V> void Visit(Operation &operation, V &v) {
			std::visit([&](auto &value) { Describe(value, v); }, operation);
		}

		// Shared no-op defaults; each visitor overrides what it needs.
		struct Visitor {
			bool LiveDocument = true;
			bool Live() const {
				return LiveDocument;
			}
			void Check(bool, std::string_view) {}
		};

		struct Validator : Visitor {
			std::string Failure;
			void Fail(std::string message) {
				if (Failure.empty()) Failure = std::move(message);
			}
			void Check(bool ok, std::string_view message) {
				if (!ok) Fail(std::string(message));
			}
			void Dimension(std::string_view, uint32_t &) {}
			void Count(std::string_view key, uint32_t &value, uint64_t minimum, uint64_t maximum) {
				if (value < minimum || value > maximum) Fail(std::string(key) + " is out of range");
			}
			void Offset(std::string_view key, int32_t &value, int64_t minimum, int64_t maximum) {
				if (value < minimum || value > maximum) Fail(std::string(key) + " is out of range");
			}
			void Number(std::string_view key, double &value, double minimum, double maximum) {
				if (!std::isfinite(value))
					Fail(std::string(key) + " must be finite");
				else if (value < minimum || value > maximum)
					Fail(
						std::string(key) + " must be within [" + FormatNumber(minimum) + "," +
						FormatNumber(maximum) + "]"
					);
			}
			void Flag(std::string_view, bool &) {}
			template <class E>
			void Choice(std::string_view key, E &value, Names names, bool = true, bool = false) {
				if (static_cast<size_t>(value) >= names.size()) Fail("unsupported " + std::string(key));
			}
			void Colour(std::string_view, std::array<uint8_t, 4> &) {}
			void Text(std::string_view key, std::string &value) {
				if (!PortableText(value)) Fail(std::string(key) + " text is too long or has control bytes");
			}
			void Stops(std::string_view, std::vector<RampStop> &) {}
			void Vector(std::string_view, std::array<double, 4> &) {}
			void Mask(std::string_view key, ChannelSet &value) {
				if (value == 0 || value > 15) Fail(std::string(key) + " must name at least one of rgba");
			}
			void Picks(std::string_view, std::array<ChannelPick, 4> &) {}
		};

		int64_t JsonInteger(const json &value, int64_t minimum, int64_t maximum) {
			if (!value.is_number_integer()) throw std::runtime_error("integer property required");
			if (value.is_number_unsigned()) {
				const uint64_t number = value.get<uint64_t>();
				if ((minimum > 0 && number < static_cast<uint64_t>(minimum)) ||
					number > static_cast<uint64_t>(maximum))
					throw std::runtime_error("integer property out of range");
				return static_cast<int64_t>(number);
			}
			const int64_t number = value.get<int64_t>();
			if (number < minimum || number > maximum)
				throw std::runtime_error("integer property out of range");
			return number;
		}
		double JsonReal(const json &value) {
			if (!value.is_number()) throw std::runtime_error("numeric property required");
			const double number = value.get<double>();
			if (!std::isfinite(number)) throw std::runtime_error("finite numeric property required");
			return number;
		}
		std::array<uint8_t, 4> ColourValue(const json &value) {
			if (!value.is_array() || value.size() != 4)
				throw std::runtime_error("colour requires four channels");
			std::array<uint8_t, 4> colour{};
			for (size_t channel = 0; channel < 4; channel++)
				colour[channel] = static_cast<uint8_t>(JsonInteger(value[channel], 0, 255));
			return colour;
		}

		// Reads each described key and refuses any extra field.
		struct Decoder : Visitor {
			const json &Properties;
			size_t Seen = 0;
			Decoder(const json &properties, bool live) : Properties(properties) {
				LiveDocument = live;
				if (!properties.is_object())
					throw std::runtime_error("object fields do not match the project schema");
			}
			const json *Field(std::string_view key, bool optional = false) {
				const auto found = Properties.find(std::string(key));
				if (found == Properties.end()) {
					if (optional) return nullptr;
					throw std::runtime_error("missing project field: " + std::string(key));
				}
				++Seen;
				return &*found;
			}
			void Finish() const {
				if (Seen != Properties.size())
					throw std::runtime_error("object fields do not match the project schema");
			}
			void Dimension(std::string_view key, uint32_t &value) {
				value = static_cast<uint32_t>(JsonInteger(*Field(key), 1, Limits::MaximumDimension));
			}
			void Count(std::string_view key, uint32_t &value, uint64_t minimum, uint64_t maximum) {
				value = static_cast<uint32_t>(JsonInteger(*Field(key), int64_t(minimum), int64_t(maximum)));
			}
			void Offset(std::string_view key, int32_t &value, int64_t minimum, int64_t maximum) {
				value = static_cast<int32_t>(JsonInteger(*Field(key), minimum, maximum));
			}
			void Number(std::string_view key, double &value, double, double) {
				value = JsonReal(*Field(key));
			}
			void Flag(std::string_view key, bool &value) {
				value = Field(key)->get<bool>();
			}
			template <class E>
			void Choice(std::string_view key, E &value, Names names, bool = true, bool optional = false) {
				const json *field = Field(key, optional);
				if (field == nullptr) return;
				const auto name = field->get<std::string>();
				const auto found = std::find(names.begin(), names.end(), name);
				if (found == names.end())
					throw std::runtime_error("unsupported " + std::string(key) + ": " + name);
				value = static_cast<E>(found - names.begin());
			}
			void Colour(std::string_view key, std::array<uint8_t, 4> &value) {
				value = ColourValue(*Field(key));
			}
			void Text(std::string_view key, std::string &value) {
				value = Field(key)->get<std::string>();
			}
			void Stops(std::string_view key, std::vector<RampStop> &value) {
				const json &stops = *Field(key);
				if (!stops.is_array() || stops.empty() || stops.size() > Limits::MaximumRampStops)
					throw std::runtime_error("ramp needs 1 to 16 stops");
				value.clear();
				for (const auto &stop : stops) {
					if (!stop.is_object() || stop.size() != 2 || !stop.contains("position") ||
						!stop.contains("colour"))
						throw std::runtime_error("ramp stop needs position and colour");
					value.push_back({JsonReal(stop.at("position")), ColourValue(stop.at("colour"))});
				}
			}
			void Vector(std::string_view key, std::array<double, 4> &value) {
				const json &vector = *Field(key);
				if (!vector.is_array() || vector.size() != 4)
					throw std::runtime_error("vector requires four numbers");
				for (size_t index = 0; index < 4; ++index)
					value[index] = JsonReal(vector[index]);
			}
			void Mask(std::string_view key, ChannelSet &value) {
				if (!ParseMask(Field(key)->get<std::string>(), value))
					throw std::runtime_error("channels must name at least one of rgba once");
			}
			void Picks(std::string_view key, std::array<ChannelPick, 4> &value) {
				const json &picks = *Field(key);
				if (!picks.is_array() || picks.size() != 4)
					throw std::runtime_error("picks require four entries");
				for (size_t index = 0; index < 4; ++index)
					if (!ParsePick(picks[index].get<std::string>(), value[index]))
						throw std::runtime_error("channel pick must be 0.r to 3.a, zero or one");
			}
		};

		struct Encoder : Visitor {
			json Out = json::object();
			void Dimension(std::string_view key, uint32_t &value) {
				Out[std::string(key)] = value;
			}
			void Count(std::string_view key, uint32_t &value, uint64_t, uint64_t) {
				Out[std::string(key)] = value;
			}
			void Offset(std::string_view key, int32_t &value, int64_t, int64_t) {
				Out[std::string(key)] = value;
			}
			void Number(std::string_view key, double &value, double, double) {
				Out[std::string(key)] = value;
			}
			void Flag(std::string_view key, bool &value) {
				Out[std::string(key)] = value;
			}
			template <class E>
			void Choice(std::string_view key, E &value, Names names, bool = true, bool optional = false) {
				const size_t index = static_cast<size_t>(value);
				if (index >= names.size()) throw std::runtime_error("unsupported " + std::string(key));
				if (optional && index == 0) return;
				Out[std::string(key)] = names[index];
			}
			void Colour(std::string_view key, std::array<uint8_t, 4> &value) {
				Out[std::string(key)] = value;
			}
			void Text(std::string_view key, std::string &value) {
				Out[std::string(key)] = value;
			}
			void Stops(std::string_view key, std::vector<RampStop> &value) {
				json stops = json::array();
				for (const auto &stop : value)
					stops.push_back({{"position", stop.Position}, {"colour", stop.Colour}});
				Out[std::string(key)] = std::move(stops);
			}
			void Vector(std::string_view key, std::array<double, 4> &value) {
				Out[std::string(key)] = value;
			}
			void Mask(std::string_view key, ChannelSet &value) {
				Out[std::string(key)] = FormatMask(value);
			}
			void Picks(std::string_view key, std::array<ChannelPick, 4> &value) {
				json picks = json::array();
				for (const auto &pick : value)
					picks.push_back(FormatPick(pick));
				Out[std::string(key)] = std::move(picks);
			}
		};

		struct Specs : Visitor {
			std::vector<PropertySpec> Out;
			void Add(std::string_view key, PropertyKind kind, double minimum = 0, double maximum = 0) {
				Out.push_back({std::string(key), kind, minimum, maximum, {}, true});
			}
			void Dimension(std::string_view key, uint32_t &) {
				Add(key, PropertyKind::Dimension, 1, Limits::MaximumDimension);
			}
			void Count(std::string_view key, uint32_t &, uint64_t minimum, uint64_t maximum) {
				Add(key, PropertyKind::Count, double(minimum), double(maximum));
			}
			void Offset(std::string_view key, int32_t &, int64_t minimum, int64_t maximum) {
				Add(key, PropertyKind::Offset, double(minimum), double(maximum));
			}
			void Number(std::string_view key, double &, double minimum, double maximum) {
				Add(key, PropertyKind::Number, minimum, maximum);
			}
			void Flag(std::string_view key, bool &) {
				Add(key, PropertyKind::Flag);
			}
			template <class E>
			void Choice(std::string_view key, E &, Names names, bool bindable = true, bool = false) {
				Add(key, PropertyKind::Choice);
				Out.back().Choices.assign(names.begin(), names.end());
				Out.back().Bindable = bindable;
			}
			void Colour(std::string_view key, std::array<uint8_t, 4> &) {
				Add(key, PropertyKind::Colour);
			}
			void Text(std::string_view key, std::string &) {
				Add(key, PropertyKind::Text);
			}
			void Stops(std::string_view key, std::vector<RampStop> &) {
				Add(key, PropertyKind::Stops);
			}
			void Vector(std::string_view key, std::array<double, 4> &) {
				Add(key, PropertyKind::Vector);
			}
			void Mask(std::string_view key, ChannelSet &) {
				Add(key, PropertyKind::ChannelSet);
			}
			void Picks(std::string_view key, std::array<ChannelPick, 4> &) {
				Add(key, PropertyKind::Picks);
			}
		};

		// Reads or writes the one property named Key through InputValue.
		struct Access : Visitor {
			std::string_view Key;
			const InputValue *Input = nullptr; // null reads into Output
			InputValue Output;
			bool Found = false;
			bool Accepted = false;
			bool Bindable = false;
			bool Match(std::string_view key) {
				if (key != Key || Found) return false;
				Found = true;
				return true;
			}
			template <class T> bool Integral(T &value, double minimum, double maximum) {
				const auto *number = std::get_if<double>(Input);
				if (!number || !std::isfinite(*number) || std::trunc(*number) != *number ||
					*number < minimum || *number > maximum)
					return false;
				value = static_cast<T>(*number);
				return true;
			}
			template <class Read, class Write> void Use(Read read, Write write) {
				Bindable = true;
				if (Input == nullptr) {
					Output = read();
					Accepted = true;
				} else
					Accepted = write();
			}
			void Dimension(std::string_view key, uint32_t &value) {
				if (!Match(key)) return;
				Use([&] { return InputValue(double(value)); },
					[&] { return Integral(value, 1, Limits::MaximumDimension); });
			}
			void Count(std::string_view key, uint32_t &value, uint64_t, uint64_t) {
				if (!Match(key)) return;
				Use([&] { return InputValue(double(value)); },
					[&] { return Integral(value, 0, UINT32_MAX); });
			}
			void Offset(std::string_view key, int32_t &value, int64_t, int64_t) {
				if (!Match(key)) return;
				Use([&] { return InputValue(double(value)); },
					[&] { return Integral(value, INT32_MIN, INT32_MAX); });
			}
			void Number(std::string_view key, double &value, double, double) {
				if (!Match(key)) return;
				Use([&] { return InputValue(value); },
					[&] {
						const auto *number = std::get_if<double>(Input);
						if (!number) return false;
						value = *number;
						return true;
					});
			}
			void Flag(std::string_view key, bool &value) {
				if (!Match(key)) return;
				Use([&] { return InputValue(value); },
					[&] {
						const auto *flag = std::get_if<bool>(Input);
						if (!flag) return false;
						value = *flag;
						return true;
					});
			}
			template <class E>
			void Choice(std::string_view key, E &value, Names names, bool bindable = true, bool = false) {
				if (!Match(key)) return;
				Use(
					[&] {
						const size_t index = static_cast<size_t>(value);
						return InputValue(std::string(index < names.size() ? names[index] : ""));
					},
					[&] {
						const auto *text = std::get_if<std::string>(Input);
						if (!text) return false;
						const auto found = std::find(names.begin(), names.end(), *text);
						if (found == names.end()) return false;
						value = static_cast<E>(found - names.begin());
						return true;
					}
				);
				Bindable = bindable;
			}
			void Colour(std::string_view key, std::array<uint8_t, 4> &value) {
				if (!Match(key)) return;
				Use([&] { return InputValue(value); },
					[&] {
						const auto *colour = std::get_if<std::array<uint8_t, 4>>(Input);
						if (!colour) return false;
						value = *colour;
						return true;
					});
			}
			template <class T, class Format, class Parse>
			void Spelled(std::string_view key, T &value, Format format, Parse parse) {
				if (!Match(key)) return;
				Use([&] { return InputValue(format(value)); },
					[&] {
						const auto *text = std::get_if<std::string>(Input);
						return text && parse(*text, value);
					});
			}
			void Text(std::string_view key, std::string &value) {
				Spelled(
					key,
					value,
					[](const std::string &text) { return text; },
					[](const std::string &text, std::string &out) {
						out = text;
						return true;
					}
				);
			}
			void Stops(std::string_view key, std::vector<RampStop> &value) {
				Spelled(key, value, FormatStops, [](const std::string &text, auto &out) {
					return ParseStops(text, out);
				});
			}
			void Vector(std::string_view key, std::array<double, 4> &value) {
				Spelled(key, value, FormatVector, [](const std::string &text, auto &out) {
					return ParseVector(text, out);
				});
			}
			void Mask(std::string_view key, ChannelSet &value) {
				Spelled(key, value, FormatMask, [](const std::string &text, auto &out) {
					return ParseMask(text, out);
				});
			}
			void Picks(std::string_view key, std::array<ChannelPick, 4> &value) {
				Spelled(key, value, FormatPicks, [](const std::string &text, auto &out) {
					return ParsePicks(text, out);
				});
			}
		};

		uint64_t PropertyFold(uint64_t signature, uint64_t word) {
			return engine::ang::HashMix(signature, word);
		}
		uint64_t PropertyFoldText(uint64_t signature, std::string_view text) {
			return engine::ang::HashText(signature, text);
		}
		struct Hasher : Visitor {
			uint64_t Signature = engine::ang::HASH_SEED;
			void Dimension(std::string_view, uint32_t &value) {
				Signature = PropertyFold(Signature, value);
			}
			void Count(std::string_view, uint32_t &value, uint64_t, uint64_t) {
				Signature = PropertyFold(Signature, value);
			}
			void Offset(std::string_view, int32_t &value, int64_t, int64_t) {
				Signature = PropertyFold(Signature, static_cast<uint32_t>(value));
			}
			void Number(std::string_view, double &value, double, double) {
				Signature = PropertyFold(Signature, std::bit_cast<uint64_t>(value));
			}
			void Flag(std::string_view, bool &value) {
				Signature = PropertyFold(Signature, value);
			}
			template <class E> void Choice(std::string_view, E &value, Names, bool = true, bool = false) {
				Signature = PropertyFold(Signature, static_cast<uint64_t>(value));
			}
			void Colour(std::string_view, std::array<uint8_t, 4> &value) {
				for (uint8_t channel : value)
					Signature = PropertyFold(Signature, channel);
			}
			void Text(std::string_view, std::string &value) {
				Signature = PropertyFoldText(Signature, value);
			}
			void Stops(std::string_view, std::vector<RampStop> &value) {
				Signature = PropertyFold(Signature, value.size());
				for (const auto &stop : value) {
					Signature = PropertyFold(Signature, std::bit_cast<uint64_t>(stop.Position));
					for (uint8_t channel : stop.Colour)
						Signature = PropertyFold(Signature, channel);
				}
			}
			void Vector(std::string_view, std::array<double, 4> &value) {
				for (double number : value)
					Signature = PropertyFold(Signature, std::bit_cast<uint64_t>(number));
			}
			void Mask(std::string_view, ChannelSet &value) {
				Signature = PropertyFold(Signature, value);
			}
			void Picks(std::string_view, std::array<ChannelPick, 4> &value) {
				for (const auto &pick : value)
					Signature =
						PropertyFold(PropertyFold(Signature, static_cast<uint8_t>(pick.Input)), pick.Channel);
			}
		};

		template <size_t Index = 0> bool DefaultFor(std::string_view kind, Operation &out) {
			if constexpr (Index < std::variant_size_v<Operation>) {
				if (NODE_KIND_IDS[Index] == kind) {
					out.emplace<Index>();
					return true;
				}
				return DefaultFor<Index + 1>(kind, out);
			} else
				return false;
		}
	}

	bool detail::SourceValid(std::string_view path) {
		if (IsEditableImageReference(path)) return true;
		if (path.empty() || path.size() > 4096 || path.front() == '/' ||
			path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos)
			return false;
		for (size_t begin = 0; begin <= path.size();) {
			const size_t end = path.find('/', begin);
			const auto segment =
				path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
			if (segment.empty() || segment == "." || segment == ".." ||
				std::any_of(segment.begin(), segment.end(), [](unsigned char character) {
					return character < 32;
				}))
				return false;
			if (end == std::string_view::npos) return true;
			begin = end + 1;
		}
		return false;
	}
	std::string detail::Validate(const Operation &operation) {
		if (operation.valueless_by_exception()) return "node has no operation";
		Operation copy = operation;
		Validator validator;
		Visit(copy, validator);
		return validator.Failure;
	}
	Operation detail::DecodeOperation(std::string_view kind, const json &properties, bool live) {
		Operation operation;
		if (!DefaultFor(kind, operation))
			throw std::runtime_error("unsupported node kind: " + std::string(kind));
		Decoder decoder(properties, live);
		Visit(operation, decoder);
		decoder.Finish();
		return operation;
	}
	json detail::EncodeOperation(const Operation &operation, bool live) {
		Operation copy = operation;
		Encoder encoder;
		encoder.LiveDocument = live;
		Visit(copy, encoder);
		return std::move(encoder.Out);
	}

	std::vector<PropertySpec> DescribeKind(std::string_view kind) {
		Operation operation;
		if (!DefaultFor(kind, operation)) return {};
		Specs specs;
		Visit(operation, specs);
		return std::move(specs.Out);
	}
	bool DefaultOperation(std::string_view kind, Operation &out) {
		Operation operation;
		if (!DefaultFor(kind, operation)) return false;
		out = std::move(operation);
		return true;
	}
	bool GetProperty(const Operation &operation, std::string_view key, InputValue &out) {
		if (operation.valueless_by_exception()) return false;
		Operation copy = operation;
		Access access;
		access.Key = key;
		Visit(copy, access);
		if (!access.Accepted) return false;
		out = std::move(access.Output);
		return true;
	}
	bool SetProperty(Operation &operation, std::string_view key, const InputValue &value) {
		if (operation.valueless_by_exception() || value.valueless_by_exception()) return false;
		Operation copy = operation;
		Access access;
		access.Key = key;
		access.Input = &value;
		Visit(copy, access);
		if (!access.Accepted) return false;
		operation = std::move(copy);
		return true;
	}
	int PropertyType(const Operation &operation, std::string_view property) noexcept {
		if (operation.valueless_by_exception()) return -1;
		try {
			Operation copy = operation;
			Access access;
			access.Key = property;
			Visit(copy, access);
			if (!access.Accepted || !access.Bindable) return -1;
			return static_cast<int>(access.Output.index());
		} catch (...) {
			return -1;
		}
	}
	uint64_t Fingerprint(const Operation &operation) noexcept {
		if (operation.valueless_by_exception()) return 0;
		try {
			Operation copy = operation;
			Hasher hasher;
			hasher.Signature = PropertyFoldText(hasher.Signature, Kind(operation));
			Visit(copy, hasher);
			return hasher.Signature;
		} catch (...) {
			return 0;
		}
	}
}
