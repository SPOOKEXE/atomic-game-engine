// The demo node set: terrain fields, a colouriser, and two slow tasks.
//
// **Copy this file, change the registrations, and nothing in `nodegraph` needs
// editing**, which is the property the whole design is for. Three kinds of
// node, each demonstrating a different half of the machinery:
//
// - **Generators and filters**: value-noise fBm, ridged noise, domain warp,
//   terraces, remap, slope, a threshold mask and a combine. These are sync: they
//   run inside `Run` and are cheap enough that a slider drag recomputes the
//   chain between frames.
// - **A colouriser**, which turns a height field into an actual picture. It is
//   what makes the thumbnails on the nodes worth having, and it is the split the
//   JavaScript template draws too: a height field is data, and turning it into
//   colour is a node somebody adds rather than something the viewer does
//   quietly.
// - **Erosion and a staged task**, which are async. Erosion is genuinely slow
//   (a few hundred milliseconds at 256²), and the staged task is slow on
//   purpose, so that two of them in two branches visibly run at once.
//
// **Every evaluation here is pure.** Results are cached against a hash of a
// node's parameters and its inputs' hashes, so a function that read a clock
// would produce a picture the cache then refuses to recompute, a stale frame
// that looks like a broken graph. The staged task sleeps rather than reading
// wall-clock time *into* its result, for the same reason.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <nodegraph/Preview.hpp>
#include <string>
#include <studio/DemoNodes.hpp>
#include <thread>
#include <vector>

namespace studio {

	// **A directive rather than twenty-odd declarations.** Every name this file
	// uses below - `NodeType`, `Port`, `Slider`, `Outputs`, `ANY_TYPE` - is the
	// node graph's vocabulary, because building that vocabulary is the whole of
	// what the file does. Listing them one at a time was noise that had to be
	// extended on every new node type. It is a source file, so nothing leaks.
	using namespace nodegraph;

	namespace {
		// A square scalar grid, in 0..1. What flows down a `data.FIELD`
		// wire. The library never learns what this is, which is what
		// `Inputs::In` exists for.
		struct Field {
			int Side = 0;
			std::vector<float> Data;

			bool Empty() const {
				return Side <= 0 || Data.empty();
			}

			float At(int x, int y) const {
				const int cx = std::clamp(x, 0, Side - 1);
				const int cy = std::clamp(y, 0, Side - 1);
				return Data[static_cast<size_t>(cy) * static_cast<size_t>(Side) + static_cast<size_t>(cx)];
			}

			float &At(int x, int y) {
				return Data[static_cast<size_t>(y) * static_cast<size_t>(Side) + static_cast<size_t>(x)];
			}
		};

		// A square colour grid. What a `data.IMAGE` wire carries, and what a
		// thumbnail is made of without further interpretation.
		struct Image {
			int Side = 0;
			std::vector<uint8_t> Rgba;

			bool Empty() const {
				return Side <= 0 || Rgba.empty();
			}
		};

		Field Made(int side) {
			Field field;
			field.Side = side;
			field.Data.assign(static_cast<size_t>(side) * static_cast<size_t>(side), 0.0f);
			return field;
		}

		// **Deterministic on purpose.** An `Evaluate` that read a clock or a
		// global would produce a result the cache then refuses to recompute.
		float Hashed(int x, int y, int seed) {
			uint32_t value = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
							 static_cast<uint32_t>(seed) * 362437u;
			value = (value ^ (value >> 13)) * 1274126177u;
			return static_cast<float>((value ^ (value >> 16)) & 0xFFFFu) / 65535.0f;
		}

		float Smooth(float t) {
			return t * t * (3.0f - 2.0f * t);
		}

		float Noise(float x, float y, int seed) {
			const int ix = static_cast<int>(std::floor(x));
			const int iy = static_cast<int>(std::floor(y));
			const float fx = Smooth(x - static_cast<float>(ix));
			const float fy = Smooth(y - static_cast<float>(iy));

			const float a = Hashed(ix, iy, seed);
			const float b = Hashed(ix + 1, iy, seed);
			const float c = Hashed(ix, iy + 1, seed);
			const float d = Hashed(ix + 1, iy + 1, seed);

			const float top = a + (b - a) * fx;
			const float bottom = c + (d - c) * fx;
			return top + (bottom - top) * fy;
		}

		// Fractional Brownian motion: octaves of the same noise, each half
		// the amplitude and twice the frequency of the one before.
		//
		// **One function for both generators**, with `ridge` deciding
		// whether each octave is folded about its middle. Two copies of an
		// octave loop is where two generators start disagreeing about what
		// `frequency` means.
		float Fractal(
			float x, float y, int seed, int octaves, float frequency, float lacunarity, float gain, bool ridge
		) {
			float total = 0.0f;
			float amplitude = 1.0f;
			float sum = 0.0f;

			for (int octave = 0; octave < octaves; octave++) {
				float sample = Noise(x * frequency, y * frequency, seed + octave * 17);
				if (ridge) {
					// Fold, so a ridge forms where the noise crosses its
					// middle: the whole difference between rolling hills
					// and a mountain range.
					sample = 1.0f - std::fabs(sample * 2.0f - 1.0f);
					sample *= sample;
				}
				total += sample * amplitude;
				sum += amplitude;
				amplitude *= gain;
				frequency *= lacunarity;
			}

			return sum > 0.0f ? total / sum : 0.0f;
		}

		int SideOf(const Inputs &in, const char *key) {
			const std::string chosen = in.Widget(key).Text;
			return chosen.empty() ? 128 : std::stoi(chosen);
		}

		// The 0..1 range a field is expected to be in, restored after an
		// operation that can leave it anywhere.
		void Normalise(Field &field) {
			if (field.Empty()) {
				return;
			}
			float low = field.Data.front();
			float high = low;
			for (const float sample : field.Data) {
				low = std::min(low, sample);
				high = std::max(high, sample);
			}
			const float span = high - low;
			if (span <= 1e-6f) {
				return;
			}
			for (float &sample : field.Data) {
				sample = (sample - low) / span;
			}
		}

		// --- pictures -------------------------------------------------------

		// How wide a thumbnail is. Bigger than the node draws it, because the
		// inspector shows the same texture large: one conversion, two sizes.
		constexpr uint32_t PREVIEW_SIDE = 160;

		// Relief shading: a surface lit from the north-west, which is what
		// makes a height field read as terrain rather than as a grey cloud.
		float Relief(const Field &field, int x, int y) {
			const float dx = field.At(x + 1, y) - field.At(x - 1, y);
			const float dy = field.At(x, y + 1) - field.At(x, y - 1);
			const float lit = 0.5f + (dx + dy) * static_cast<float>(field.Side) * 0.06f;
			return std::clamp(lit, 0.25f, 1.0f);
		}

		bool PreviewOfField(const std::any &payload, PreviewImage &image) {
			const Field *field = std::any_cast<Field>(&payload);
			if (field == nullptr || field->Empty()) {
				return false;
			}

			image.Side = PREVIEW_SIDE;
			image.Rgba.assign(static_cast<size_t>(PREVIEW_SIDE) * PREVIEW_SIDE * 4, 0);

			for (uint32_t y = 0; y < PREVIEW_SIDE; y++) {
				for (uint32_t x = 0; x < PREVIEW_SIDE; x++) {
					// Nearest sample. A thumbnail is a glance, and a
					// bilinear one costs four reads a pixel to say the same
					// thing at this size.
					const int sx = static_cast<int>(
						static_cast<float>(x) / PREVIEW_SIDE * static_cast<float>(field->Side)
					);
					const int sy = static_cast<int>(
						static_cast<float>(y) / PREVIEW_SIDE * static_cast<float>(field->Side)
					);

					const float height = std::clamp(field->At(sx, sy), 0.0f, 1.0f);
					const float shade = height * Relief(*field, sx, sy);
					const auto grey = static_cast<uint8_t>(std::clamp(shade, 0.0f, 1.0f) * 255.0f);

					const size_t at = (static_cast<size_t>(y) * PREVIEW_SIDE + x) * 4;
					image.Rgba[at] = grey;
					image.Rgba[at + 1] = grey;
					image.Rgba[at + 2] = grey;
					image.Rgba[at + 3] = 255;
				}
			}
			return true;
		}

		bool PreviewOfImage(const std::any &payload, PreviewImage &image) {
			const Image *source = std::any_cast<Image>(&payload);
			if (source == nullptr || source->Empty()) {
				return false;
			}

			image.Side = PREVIEW_SIDE;
			image.Rgba.assign(static_cast<size_t>(PREVIEW_SIDE) * PREVIEW_SIDE * 4, 0);

			for (uint32_t y = 0; y < PREVIEW_SIDE; y++) {
				for (uint32_t x = 0; x < PREVIEW_SIDE; x++) {
					const int sx = static_cast<int>(
						static_cast<float>(x) / PREVIEW_SIDE * static_cast<float>(source->Side)
					);
					const int sy = static_cast<int>(
						static_cast<float>(y) / PREVIEW_SIDE * static_cast<float>(source->Side)
					);
					const size_t from = (static_cast<size_t>(sy) * static_cast<size_t>(source->Side) +
										 static_cast<size_t>(sx)) *
										4;
					const size_t at = (static_cast<size_t>(y) * PREVIEW_SIDE + x) * 4;
					for (size_t channel = 0; channel < 4; channel++) {
						image.Rgba[at + channel] = source->Rgba[from + channel];
					}
				}
			}
			return true;
		}

		// --- what a wire says about itself ----------------------------------
		//
		// **On the data type rather than on each node**, which is the whole
		// point of `DataType::Preview`: eight filters carry a field and one
		// function draws every one of them. And, more usefully, an
		// inspector can draw a node's *inputs*, whose payloads were made
		// upstream by a type it has never heard of.

		std::string DescribeField(const std::any &payload) {
			const Field *field = std::any_cast<Field>(&payload);
			if (field == nullptr || field->Empty()) {
				return "an empty field";
			}

			float low = field->Data.front();
			float high = low;
			for (const float sample : field->Data) {
				low = std::min(low, sample);
				high = std::max(high, sample);
			}

			char text[64];
			std::snprintf(
				text,
				sizeof(text),
				"scalar %d\xC2\xB2  %.3f - %.3f",
				field->Side,
				static_cast<double>(low),
				static_cast<double>(high)
			);
			return text;
		}

		// A field read as elevation, for the 3-D view.
		//
		// **The raw numbers, not the thumbnail.** `PreviewOfField` has
		// already lit its picture, and reading that back as height would put
		// the shading into the geometry.
		bool HeightsOfField(const std::any &payload, Surface &out) {
			const Field *field = std::any_cast<Field>(&payload);
			if (field == nullptr || field->Empty() || field->Side < 2) {
				return false;
			}
			out.Side = static_cast<uint32_t>(field->Side);
			out.Heights.assign(field->Data.begin(), field->Data.end());
			return true;
		}

		std::string DescribeImage(const std::any &payload) {
			const Image *image = std::any_cast<Image>(&payload);
			if (image == nullptr || image->Empty()) {
				return "an empty picture";
			}
			char text[32];
			std::snprintf(text, sizeof(text), "rgb %d\xC2\xB2", image->Side);
			return text;
		}

		std::string DescribeNumber(const std::any &payload) {
			const double *number = std::any_cast<double>(&payload);
			if (number == nullptr) {
				return "not a number";
			}
			char text[32];
			std::snprintf(text, sizeof(text), "%.4f", *number);
			return text;
		}

		bool Registered = false;
	}

	void RegisterDemoNodes() {
		// Idempotent: the panel registers on first open and every test case
		// asks. A second registration replaces in place, so this is a saving
		// rather than a correctness fix. But a palette that grew a
		// duplicate row every open would be neither.
		if (Registered) {
			return;
		}
		Registered = true;

		// **Each type is registered with how to draw and how to read it**,
		// which is what every preview in this panel goes through. A number
		// gets no picture on purpose: a grey square standing for `0.4213` is
		// worse than the number itself.
		DataType number;
		number.Id = "data.NUMBER";
		number.Label = "Number";
		number.Tint = Colour::Hex(0xF2C14E);
		number.Description = "One scalar";
		number.Describe = DescribeNumber;
		DataTypes::Register(number);

		DataType field;
		field.Id = "data.FIELD";
		field.Label = "Field";
		field.Tint = Colour::Hex(0x4CA6FF);
		field.Description = "A square grid of heights, 0..1";
		field.Preview = PreviewOfField;
		field.Describe = DescribeField;
		field.Heights = HeightsOfField;
		DataTypes::Register(field);

		DataType picture;
		picture.Id = "data.IMAGE";
		picture.Label = "Image";
		picture.Tint = Colour::Hex(0xE06C9F);
		picture.Description = "A square grid of colours";
		picture.Preview = PreviewOfImage;
		picture.Describe = DescribeImage;
		DataTypes::Register(picture);

		// --- numbers --------------------------------------------------------

		NodeType constant;
		constant.Id = "number.constant";
		constant.Title = "Number";
		constant.Category = "Input";
		constant.Accent = Colour::Hex(0xF2C14E);
		constant.Outputs = {Port("Out", "data.NUMBER")};
		constant.Widgets = {Number("value", "Value", 1.0)};
		constant.Evaluate = [](const Inputs &in) { return Outputs{{"Out", in.Real("value")}}; };
		NodeTypes::Register(constant);

		NodeType arithmetic;
		arithmetic.Id = "number.arithmetic";
		arithmetic.Title = "Arithmetic";
		arithmetic.Category = "Maths";
		arithmetic.Accent = Colour::Hex(0xE58C4A);
		arithmetic.Inputs = {Port("A", "data.NUMBER"), Port("B", "data.NUMBER")};
		arithmetic.Outputs = {Port("Out", "data.NUMBER")};
		arithmetic.Widgets = {
			Select("op", "Operation", {"add", "subtract", "multiply", "divide"}, 0),
			Toggle("absolute", "Absolute", false),
		};
		arithmetic.Evaluate = [](const Inputs &in) {
			const double a = in.In<double>("A", 0.0);
			const double b = in.In<double>("B", 0.0);
			const std::string op = in.Widget("op").Text;

			double result = a + b;
			if (op == "subtract") {
				result = a - b;
			} else if (op == "multiply") {
				result = a * b;
			} else if (op == "divide") {
				// Zero rather than an infinity: an infinity propagates into
				// a field, a picture and a saved file, and the first place
				// anybody notices is a blank one.
				result = b == 0.0 ? 0.0 : a / b;
			}
			if (in.Widget("absolute").Flag) {
				result = std::fabs(result);
			}
			return Outputs{{"Out", result}};
		};
		NodeTypes::Register(arithmetic);

		// --- generators -----------------------------------------------------

		NodeType perlin;
		perlin.Id = "field.perlin";
		perlin.Title = "Noise";
		perlin.Category = "Generate";
		perlin.Accent = Colour::Hex(0x4CA6FF);
		perlin.Subtitle = "value fBm";
		perlin.Inputs = {Port("Frequency", "data.NUMBER")};
		perlin.Outputs = {Port("Out", "data.FIELD")};
		perlin.Widgets = {
			Select("resolution", "Resolution", {"64", "128", "256", "512"}, 0),
			Slider("frequency", "Frequency", 1.0, 16.0, 4.0),
			Slider("octaves", "Octaves", 1.0, 8.0, 5.0),
			Slider("gain", "Gain", 0.2, 0.8, 0.5),
			Number("seed", "Seed", 1.0),
		};
		perlin.Evaluate = [](const Inputs &in) {
			const int side = SideOf(in, "resolution");
			const auto seed = static_cast<int>(in.Real("seed"));
			const auto octaves = static_cast<int>(in.Real("octaves"));
			const auto gain = static_cast<float>(in.Real("gain"));

			// **A wire beats the knob.** The knob is what the node does on
			// its own; a wire is somebody saying otherwise, and a node that
			// ignored it would be a wire that does nothing.
			const auto frequency = static_cast<float>(in.In<double>("Frequency", in.Real("frequency")));

			Field field = Made(side);
			for (int y = 0; y < side; y++) {
				for (int x = 0; x < side; x++) {
					const float u = static_cast<float>(x) / static_cast<float>(side);
					const float v = static_cast<float>(y) / static_cast<float>(side);
					field.At(x, y) = Fractal(u, v, seed, octaves, frequency, 2.0f, gain, false);
				}
			}
			Normalise(field);
			return Outputs{{"Out", field}};
		};
		NodeTypes::Register(perlin);

		NodeType ridged = perlin;
		ridged.Id = "field.ridged";
		ridged.Title = "Ridged";
		ridged.Subtitle = "folded fBm, ranges";
		ridged.Accent = Colour::Hex(0x3D8BD6);
		ridged.Evaluate = [](const Inputs &in) {
			const int side = SideOf(in, "resolution");
			const auto seed = static_cast<int>(in.Real("seed"));
			const auto octaves = static_cast<int>(in.Real("octaves"));
			const auto gain = static_cast<float>(in.Real("gain"));
			const auto frequency = static_cast<float>(in.In<double>("Frequency", in.Real("frequency")));

			Field field = Made(side);
			for (int y = 0; y < side; y++) {
				for (int x = 0; x < side; x++) {
					const float u = static_cast<float>(x) / static_cast<float>(side);
					const float v = static_cast<float>(y) / static_cast<float>(side);
					field.At(x, y) = Fractal(u, v, seed, octaves, frequency, 2.0f, gain, true);
				}
			}
			Normalise(field);
			return Outputs{{"Out", field}};
		};
		NodeTypes::Register(ridged);

		// --- filters --------------------------------------------------------

		NodeType warp;
		warp.Id = "field.warp";
		warp.Title = "Domain Warp";
		warp.Category = "Filter";
		warp.Accent = Colour::Hex(0x6FCF97);
		warp.Subtitle = "displaces by another field";
		warp.Inputs = {Port("In", "data.FIELD"), Port("By", "data.FIELD")};
		warp.Outputs = {Port("Out", "data.FIELD")};
		warp.Widgets = {Slider("amount", "Amount", 0.0, 0.5, 0.15)};
		warp.Evaluate = [](const Inputs &in) {
			const Field source = in.In<Field>("In");
			const Field by = in.In<Field>("By");
			if (source.Empty()) {
				return Outputs{{"Out", source}};
			}

			const auto amount = static_cast<float>(in.Real("amount")) * static_cast<float>(source.Side);
			Field out = Made(source.Side);

			for (int y = 0; y < source.Side; y++) {
				for (int x = 0; x < source.Side; x++) {
					// **Its own noise when nothing is wired in.** A warp
					// with no second field would otherwise be an expensive
					// copy, and the node would look broken rather than
					// unconnected.
					const float offset =
						by.Empty() ? Noise(static_cast<float>(x) * 0.05f, static_cast<float>(y) * 0.05f, 99)
								   : by.At(x, y);
					const auto shift = static_cast<int>((offset - 0.5f) * amount);
					out.At(x, y) = source.At(x + shift, y + shift);
				}
			}
			return Outputs{{"Out", out}};
		};
		NodeTypes::Register(warp);

		NodeType terrace;
		terrace.Id = "field.terrace";
		terrace.Title = "Terrace";
		terrace.Category = "Filter";
		terrace.Accent = Colour::Hex(0x6FCF97);
		terrace.Inputs = {Port("In", "data.FIELD")};
		terrace.Outputs = {Port("Out", "data.FIELD")};
		terrace.Widgets = {
			Slider("steps", "Steps", 2.0, 24.0, 8.0),
			Slider("sharpness", "Sharpness", 0.0, 1.0, 0.6),
		};
		terrace.Evaluate = [](const Inputs &in) {
			Field field = in.In<Field>("In");
			if (field.Empty()) {
				return Outputs{{"Out", field}};
			}

			const auto steps = std::max(2.0f, static_cast<float>(in.Real("steps")));
			const auto sharpness = static_cast<float>(in.Real("sharpness"));

			for (float &sample : field.Data) {
				const float scaled = sample * steps;
				const float step = std::floor(scaled);
				const float within = scaled - step;

				// Between the flat step and the untouched slope, so
				// sharpness is a blend rather than a switch: a terrace that
				// snapped at 1 and did nothing at 0.99 would be a knob with
				// one useful position.
				const float flattened = (step + std::pow(within, 1.0f + sharpness * 6.0f)) / steps;
				sample = flattened * sharpness + sample * (1.0f - sharpness);
			}
			return Outputs{{"Out", field}};
		};
		NodeTypes::Register(terrace);

		NodeType slope;
		slope.Id = "field.slope";
		slope.Title = "Slope";
		slope.Category = "Filter";
		slope.Accent = Colour::Hex(0x9BD16F);
		slope.Subtitle = "steepness, 0..1";
		slope.Inputs = {Port("In", "data.FIELD")};
		slope.Outputs = {Port("Out", "data.FIELD")};
		slope.Evaluate = [](const Inputs &in) {
			const Field source = in.In<Field>("In");
			if (source.Empty()) {
				return Outputs{{"Out", source}};
			}

			Field out = Made(source.Side);
			for (int y = 0; y < source.Side; y++) {
				for (int x = 0; x < source.Side; x++) {
					const float dx = source.At(x + 1, y) - source.At(x - 1, y);
					const float dy = source.At(x, y + 1) - source.At(x, y - 1);
					out.At(x, y) = std::sqrt(dx * dx + dy * dy) * static_cast<float>(source.Side) * 0.5f;
				}
			}
			Normalise(out);
			return Outputs{{"Out", out}};
		};
		NodeTypes::Register(slope);

		NodeType mask;
		mask.Id = "field.mask";
		mask.Title = "Threshold";
		mask.Category = "Filter";
		mask.Accent = Colour::Hex(0x9BD16F);
		mask.Inputs = {Port("In", "data.FIELD"), Port("Level", "data.NUMBER")};
		mask.Outputs = {Port("Out", "data.FIELD")};
		mask.Widgets = {
			Slider("level", "Level", 0.0, 1.0, 0.5),
			Slider("softness", "Softness", 0.0, 0.5, 0.08),
			Toggle("invert", "Invert", false),
		};
		mask.Evaluate = [](const Inputs &in) {
			Field field = in.In<Field>("In");
			if (field.Empty()) {
				return Outputs{{"Out", field}};
			}

			const auto level = static_cast<float>(in.In<double>("Level", in.Real("level")));
			const auto softness = std::max(1e-4f, static_cast<float>(in.Real("softness")));
			const bool invert = in.Widget("invert").Flag;

			for (float &sample : field.Data) {
				float value = std::clamp((sample - (level - softness)) / (softness * 2.0f), 0.0f, 1.0f);
				value = value * value * (3.0f - 2.0f * value);
				sample = invert ? 1.0f - value : value;
			}
			return Outputs{{"Out", field}};
		};
		NodeTypes::Register(mask);

		NodeType combine;
		combine.Id = "field.combine";
		combine.Title = "Combine";
		combine.Category = "Filter";
		combine.Accent = Colour::Hex(0x6FCF97);
		combine.Inputs = {
			Port("A", "data.FIELD"),
			Port("B", "data.FIELD"),
			Port("Mask", "data.FIELD"),
			Port("Amount", "data.NUMBER"),
		};
		combine.Outputs = {Port("Out", "data.FIELD")};
		combine.Widgets = {
			Select("mode", "Mode", {"blend", "add", "multiply", "minimum", "maximum"}, 0),
			Slider("amount", "Amount", 0.0, 1.0, 0.5),
		};
		combine.Evaluate = [](const Inputs &in) {
			const Field a = in.In<Field>("A");
			const Field b = in.In<Field>("B");
			const Field masked = in.In<Field>("Mask");

			if (a.Empty()) {
				return Outputs{{"Out", b}};
			}
			if (b.Side != a.Side) {
				// **Mismatched sizes refuse rather than resample.** Silently
				// stretching one onto the other is a decision the graph
				// should be making with a node somebody can see.
				return Outputs{{"Out", a}};
			}

			const auto amount = static_cast<float>(in.In<double>("Amount", in.Real("amount")));
			const std::string mode = in.Widget("mode").Text;
			const bool usable = masked.Side == a.Side;

			Field out = Made(a.Side);
			for (int y = 0; y < a.Side; y++) {
				for (int x = 0; x < a.Side; x++) {
					const float left = a.At(x, y);
					const float right = b.At(x, y);

					float mixed = left * (1.0f - amount) + right * amount;
					if (mode == "add") {
						mixed = left + right * amount;
					} else if (mode == "multiply") {
						mixed = left * (1.0f - amount + right * amount);
					} else if (mode == "minimum") {
						mixed = std::min(left, right);
					} else if (mode == "maximum") {
						mixed = std::max(left, right);
					}

					// A mask decides *where* the combine applies, which is
					// the difference between blending two terrains and
					// putting one of them in a valley.
					const float weight = usable ? masked.At(x, y) : 1.0f;
					out.At(x, y) = left * (1.0f - weight) + mixed * weight;
				}
			}
			return Outputs{{"Out", out}};
		};
		NodeTypes::Register(combine);

		// --- colour ---------------------------------------------------------

		NodeType colourise;
		colourise.Id = "image.colourise";
		colourise.Title = "Colourise";
		colourise.Category = "Colour";
		colourise.Accent = Colour::Hex(0xE06C9F);
		colourise.Subtitle = "height becomes a picture";
		colourise.Inputs = {Port("In", "data.FIELD"), Port("Mask", "data.FIELD")};
		colourise.Outputs = {Port("Out", "data.IMAGE")};
		colourise.Widgets = {
			Select("palette", "Palette", {"alpine", "desert", "volcanic", "depth"}, 0),
			Slider("sea", "Sea level", 0.0, 0.8, 0.32),
			Toggle("relief", "Relief", true),
		};
		colourise.Evaluate = [](const Inputs &in) {
			const Field field = in.In<Field>("In");
			if (field.Empty()) {
				return Outputs{{"Out", Image{}}};
			}

			const std::string palette = in.Widget("palette").Text;
			const auto sea = static_cast<float>(in.Real("sea"));
			const bool relief = in.Widget("relief").Flag;
			const Field cliffs = in.In<Field>("Mask");
			const bool usable = cliffs.Side == field.Side;

			// **Stacked bands rather than a gradient texture.** Each is a
			// colour and a boundary, which is what an author actually
			// adjusts, and it keeps the whole node four numbers wide
			// instead of needing a curve editor this canvas does not have.
			struct Band {
				float Until;
				float R;
				float G;
				float B;
			};

			std::vector<Band> bands;
			if (palette == "desert") {
				bands = {
					{0.36f, 0.76f, 0.66f, 0.42f},
					{0.55f, 0.84f, 0.71f, 0.45f},
					{0.78f, 0.66f, 0.50f, 0.34f},
					{1.01f, 0.94f, 0.90f, 0.84f}
				};
			} else if (palette == "volcanic") {
				bands = {
					{0.34f, 0.12f, 0.10f, 0.12f},
					{0.58f, 0.28f, 0.20f, 0.20f},
					{0.80f, 0.62f, 0.20f, 0.12f},
					{1.01f, 0.98f, 0.82f, 0.36f}
				};
			} else if (palette == "depth") {
				bands = {
					{0.30f, 0.02f, 0.10f, 0.28f},
					{0.55f, 0.06f, 0.32f, 0.56f},
					{0.80f, 0.24f, 0.62f, 0.78f},
					{1.01f, 0.86f, 0.96f, 1.00f}
				};
			} else {
				bands = {
					{0.38f, 0.36f, 0.52f, 0.30f},
					{0.62f, 0.28f, 0.44f, 0.24f},
					{0.82f, 0.48f, 0.44f, 0.40f},
					{1.01f, 0.96f, 0.96f, 0.98f}
				};
			}

			Image image;
			image.Side = field.Side;
			image.Rgba.assign(static_cast<size_t>(field.Side) * static_cast<size_t>(field.Side) * 4, 0);

			for (int y = 0; y < field.Side; y++) {
				for (int x = 0; x < field.Side; x++) {
					const float height = std::clamp(field.At(x, y), 0.0f, 1.0f);
					float r = 0.0f;
					float g = 0.0f;
					float b = 0.0f;

					if (height < sea) {
						// Water, darkening with depth. The sea level is a
						// knob rather than a node because every palette here
						// has one and a graph would carry the same wire four
						// times.
						const float depth = sea > 0.0f ? height / sea : 0.0f;
						r = 0.04f + depth * 0.06f;
						g = 0.12f + depth * 0.22f;
						b = 0.28f + depth * 0.34f;
					} else {
						for (const Band &band : bands) {
							if (height <= band.Until) {
								r = band.R;
								g = band.G;
								b = band.B;
								break;
							}
						}
					}

					float lit = relief ? Relief(field, x, y) : 1.0f;
					if (usable) {
						// A mask darkens where it is set: a cliff layer, in
						// the reference implementation's terms.
						lit *= 1.0f - cliffs.At(x, y) * 0.55f;
					}

					const size_t at =
						(static_cast<size_t>(y) * static_cast<size_t>(field.Side) + static_cast<size_t>(x)) *
						4;
					image.Rgba[at] = static_cast<uint8_t>(std::clamp(r * lit, 0.0f, 1.0f) * 255.0f);
					image.Rgba[at + 1] = static_cast<uint8_t>(std::clamp(g * lit, 0.0f, 1.0f) * 255.0f);
					image.Rgba[at + 2] = static_cast<uint8_t>(std::clamp(b * lit, 0.0f, 1.0f) * 255.0f);
					image.Rgba[at + 3] = 255;
				}
			}
			return Outputs{{"Out", image}};
		};
		NodeTypes::Register(colourise);

		// --- the async half -------------------------------------------------

		NodeType erode;
		erode.Id = "field.erode";
		erode.Title = "Erode";
		erode.Category = "Simulate";
		erode.Accent = Colour::Hex(0xD98C4A);
		erode.Subtitle = "thermal, then hydraulic";
		erode.Inputs = {Port("In", "data.FIELD")};
		erode.Outputs = {Port("Out", "data.FIELD")};
		erode.Widgets = {
			Slider("thermal", "Thermal passes", 0.0, 120.0, 40.0),
			Slider("talus", "Talus", 0.001, 0.05, 0.008),
			Slider("hydraulic", "Rain passes", 0.0, 60.0, 20.0),
		};

		// **Async, because it is genuinely slow.** Forty thermal passes over
		// a 256² field is tens of millions of samples; running it inside the
		// frame would stall the editor every time a slider moved, which is
		// exactly the case the async path exists for.
		erode.Async = true;
		erode.Steps = {"reading", "thermal", "rain", "settling"};
		erode.Evaluate = [](const Inputs &in) {
			Field field = in.In<Field>("In");
			if (field.Empty()) {
				return Outputs{{"Out", field}};
			}

			const auto thermal = static_cast<int>(in.Real("thermal"));
			const auto rain = static_cast<int>(in.Real("hydraulic"));
			const auto talus = static_cast<float>(in.Real("talus"));
			const float total = static_cast<float>(std::max(1, thermal + rain));

			if (in.Report) {
				in.Report(0, 0.0f, "reading");
			}

			// Thermal: material above the angle of repose slides downhill,
			// a quarter of the excess to each lower neighbour.
			for (int pass = 0; pass < thermal; pass++) {
				if (in.Cancelled()) {
					return Outputs{{"Out", field}};
				}

				Field next = field;
				for (int y = 1; y < field.Side - 1; y++) {
					for (int x = 1; x < field.Side - 1; x++) {
						const float here = field.At(x, y);
						float moved = 0.0f;

						const int dx[] = {1, -1, 0, 0};
						const int dy[] = {0, 0, 1, -1};
						for (int side = 0; side < 4; side++) {
							const float difference = here - field.At(x + dx[side], y + dy[side]);
							if (difference > talus) {
								const float slide = (difference - talus) * 0.25f;
								next.At(x + dx[side], y + dy[side]) += slide;
								moved += slide;
							}
						}
						next.At(x, y) = next.At(x, y) - moved;
					}
				}
				field = std::move(next);

				if (in.Report) {
					in.Report(1, static_cast<float>(pass + 1) / total, "thermal");
				}
			}

			// Hydraulic, in the crudest form that still carves: rain
			// dissolves, water runs to the lowest neighbour, sediment lands.
			for (int pass = 0; pass < rain; pass++) {
				if (in.Cancelled()) {
					return Outputs{{"Out", field}};
				}

				for (int y = 1; y < field.Side - 1; y++) {
					for (int x = 1; x < field.Side - 1; x++) {
						int lowestX = x;
						int lowestY = y;
						float lowest = field.At(x, y);

						for (int oy = -1; oy <= 1; oy++) {
							for (int ox = -1; ox <= 1; ox++) {
								if (field.At(x + ox, y + oy) < lowest) {
									lowest = field.At(x + ox, y + oy);
									lowestX = x + ox;
									lowestY = y + oy;
								}
							}
						}

						if (lowestX == x && lowestY == y) {
							continue;
						}
						const float carried = std::min(0.008f, (field.At(x, y) - lowest) * 0.5f);
						field.At(x, y) -= carried;
						field.At(lowestX, lowestY) += carried * 0.8f;
					}
				}

				if (in.Report) {
					in.Report(2, static_cast<float>(thermal + pass + 1) / total, "rain");
				}
			}

			if (in.Report) {
				in.Report(3, 1.0f, "settling");
			}
			Normalise(field);
			return Outputs{{"Out", field}};
		};
		NodeTypes::Register(erode);

		NodeType staged;
		staged.Id = "task.staged";
		staged.Title = "Staged Task";
		staged.Category = "Simulate";
		staged.Accent = Colour::Hex(0xB07AD6);
		staged.Subtitle = "five stages, off the frame";
		staged.Inputs = {Port("After", ANY_TYPE)};
		staged.Outputs = {Port("Done", "data.NUMBER")};
		staged.Widgets = {
			Slider("seconds", "Seconds", 0.2, 6.0, 2.0),
			Text("label", "Label", "task"),
		};
		staged.Async = true;
		staged.Steps = {"planning", "fetching", "thinking", "writing", "checking"};

		// **Slow on purpose, and it is the honest way to show concurrency.**
		// Two of these in two branches of a graph run at once because the
		// evaluator starts every node whose inputs are ready. There is no
		// scheduler deciding it. Sleeping rather than spinning, so watching
		// the demo does not cost a core.
		staged.Evaluate = [](const Inputs &in) {
			const auto seconds = static_cast<float>(in.Real("seconds"));
			const auto perStep = std::chrono::duration<float>(seconds / 5.0f);
			const char *names[] = {"planning", "fetching", "thinking", "writing", "checking"};

			for (size_t step = 0; step < 5; step++) {
				// **In slices, so a cancelled run stops within a frame or
				// two** rather than at the end of a whole stage.
				for (int slice = 0; slice < 10; slice++) {
					if (in.Cancelled()) {
						return Outputs{{"Done", 0.0}};
					}
					std::this_thread::sleep_for(perStep / 10);
					if (in.Report) {
						const float within = static_cast<float>(slice + 1) / 10.0f;
						in.Report(step, (static_cast<float>(step) + within) / 5.0f, names[step]);
					}
				}
			}
			return Outputs{{"Done", 1.0}};
		};
		NodeTypes::Register(staged);

		// --- output ---------------------------------------------------------

		NodeType readout;
		readout.Id = "field.readout";
		readout.Title = "Readout";
		readout.Category = "Output";
		readout.Accent = Colour::Hex(0x8A8A8A);
		readout.Subtitle = "the field's average";
		readout.Inputs = {Port("In", "data.FIELD")};
		readout.Outputs = {Port("Value", "data.NUMBER")};
		readout.Evaluate = [](const Inputs &in) {
			const Field field = in.In<Field>("In");
			if (field.Empty()) {
				return Outputs{{"Value", 0.0}};
			}
			double total = 0.0;
			for (const float sample : field.Data) {
				total += static_cast<double>(sample);
			}
			return Outputs{{"Value", total / static_cast<double>(field.Data.size())}};
		};
		NodeTypes::Register(readout);

		NodeType note;
		note.Id = "graph.note";
		note.Title = "Note";
		note.Category = "Output";
		note.Accent = Colour::Hex(0x777777);
		note.Inputs = {Port("Anything", ANY_TYPE)};
		note.Widgets = {Text("note", "Note", "a wildcard input takes any wire")};
		// No `Evaluate`. A node may exist only to be somewhere a wire ends;
		// the evaluator counts it as skipped and everything else treats it
		// exactly as it treats the rest.
		NodeTypes::Register(note);
	}

	void BuildDemoGraph(Graph &graph) {
		RegisterDemoNodes();
		graph.Clear();

		// A terrain chain on top, an async pair below it. Between them they
		// use every part: two generators, a warp, a combine with a mask, an
		// erosion that runs off the frame, a colouriser that makes the
		// picture, and two staged tasks in branches that do not feed each
		// other.
		const NodeId base = graph.Add("field.perlin", 40.0f, 60.0f);
		const NodeId ranges = graph.Add("field.ridged", 40.0f, 340.0f);
		const NodeId warp = graph.Add("field.warp", 300.0f, 60.0f);
		const NodeId blend = graph.Add("field.combine", 560.0f, 120.0f);
		const NodeId eroded = graph.Add("field.erode", 830.0f, 120.0f);
		const NodeId steep = graph.Add("field.slope", 1090.0f, 60.0f);
		const NodeId picture = graph.Add("image.colourise", 1090.0f, 360.0f);
		const NodeId average = graph.Add("field.readout", 1350.0f, 60.0f);

		const NodeId first = graph.Add("task.staged", 300.0f, 700.0f);
		const NodeId second = graph.Add("task.staged", 300.0f, 900.0f);
		const NodeId both = graph.Add("number.arithmetic", 620.0f, 780.0f);

		graph.Find(ranges)->Widgets["seed"].Number = 7.0;
		graph.Find(ranges)->Widgets["frequency"].Number = 6.0;
		graph.Find(blend)->Widgets["mode"].Text = "maximum";
		graph.Find(eroded)->Widgets["thermal"].Number = 30.0;
		graph.Find(first)->Widgets["label"].Text = "left";
		graph.Find(second)->Widgets["label"].Text = "right";
		graph.Find(second)->Widgets["seconds"].Number = 3.0;

		graph.Connect(base, "Out", warp, "In");
		graph.Connect(ranges, "Out", warp, "By");
		graph.Connect(warp, "Out", blend, "A");
		graph.Connect(ranges, "Out", blend, "B");
		graph.Connect(blend, "Out", eroded, "In");
		graph.Connect(eroded, "Out", steep, "In");
		graph.Connect(eroded, "Out", picture, "In");
		graph.Connect(steep, "Out", picture, "Mask");
		graph.Connect(steep, "Out", average, "In");

		graph.Connect(first, "Done", both, "A");
		graph.Connect(second, "Done", both, "B");
	}
}
