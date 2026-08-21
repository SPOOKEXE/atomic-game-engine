#include "Fixture.hpp"

#include <engine/nodegraph/Registry.hpp>
#include <engine/nodegraph/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace fixture {

	using namespace engine::nodegraph;

	namespace {

		// A field of a stated size whose samples are a cheap function of the
		// seed. Deterministic, because a result is cached against a hash and a
		// fixture that returned something new each call would make every cache
		// case fail for the wrong reason.
		Field Make(uint32_t side, double seed) {
			Field field;
			field.Side = std::max(side, 2u);
			field.Samples.resize(static_cast<size_t>(field.Side) * field.Side);
			for (uint32_t y = 0; y < field.Side; y++) {
				for (uint32_t x = 0; x < field.Side; x++) {
					const double value = std::fmod((x * 0.125) + (y * 0.0625) + seed, 1.0);
					field.Samples[static_cast<size_t>(y) * field.Side + x] = static_cast<float>(value);
				}
			}
			return field;
		}

		uint32_t SideOf(const Inputs &in, const char *key) {
			const std::string text = in.Widget(key).Text;
			const int side = text.empty() ? 32 : std::atoi(text.c_str());
			return side > 1 ? static_cast<uint32_t>(side) : 32u;
		}

		// A grey ramp. Enough for `PreviewImage::Valid` and for a case to check
		// that a picture came from the *wire* rather than from the node.
		bool FieldPicture(const std::any &payload, PreviewImage &out) {
			const Field *field = std::any_cast<Field>(&payload);
			if (field == nullptr || field->Side == 0) {
				return false;
			}
			out.Side = field->Side;
			out.Rgba.assign(static_cast<size_t>(out.Side) * out.Side * 4, 0);
			for (size_t index = 0; index < static_cast<size_t>(out.Side) * out.Side; index++) {
				const auto grey =
					static_cast<uint8_t>(std::clamp(field->Samples[index], 0.0f, 1.0f) * 255.0f);
				out.Rgba[index * 4 + 0] = grey;
				out.Rgba[index * 4 + 1] = grey;
				out.Rgba[index * 4 + 2] = grey;
				out.Rgba[index * 4 + 3] = 255;
			}
			return true;
		}

		bool FieldHeights(const std::any &payload, Surface &out) {
			const Field *field = std::any_cast<Field>(&payload);
			if (field == nullptr || field->Side < 2) {
				return false;
			}
			out.Side = field->Side;
			out.Heights = field->Samples;
			return true;
		}

		void RegisterDataTypes() {
			DataType number;
			number.Id = "data.NUMBER";
			number.Label = "Number";
			number.Tint = Colour::Hex(0xE58C4A);
			// No `Preview`: a payload carrying a number has no picture, and the
			// honest answer is none rather than a grey square.
			number.Describe = [](const std::any &payload) {
				const double *held = std::any_cast<double>(&payload);
				return held != nullptr ? std::to_string(*held) : std::string{};
			};
			DataTypes::Register(number);

			DataType field;
			field.Id = "data.FIELD";
			field.Label = "Field";
			field.Tint = Colour::Hex(0x4CA6FF);
			field.Preview = FieldPicture;
			field.Heights = FieldHeights;
			field.Describe = [](const std::any &payload) {
				const Field *held = std::any_cast<Field>(&payload);
				return held != nullptr ? "field " + std::to_string(held->Side) : std::string{};
			};
			DataTypes::Register(field);

			DataType picture;
			picture.Id = "data.IMAGE";
			picture.Label = "Image";
			picture.Tint = Colour::Hex(0xB07CE8);
			picture.Preview = [](const std::any &payload, PreviewImage &out) {
				const PreviewImage *held = std::any_cast<PreviewImage>(&payload);
				if (held == nullptr || !held->Valid()) {
					return false;
				}
				out = *held;
				return true;
			};
			DataTypes::Register(picture);
		}

		void RegisterNumberNodes() {
			NodeType constant;
			constant.Id = "number.constant";
			constant.Title = "Constant";
			constant.Category = "Maths";
			constant.Outputs = {Port("Out", "data.NUMBER")};
			constant.Widgets = {Number("value", "Value", 1.0)};
			constant.Evaluate = [](const Inputs &in) { return Outputs{{"Out", in.Real("value")}}; };
			NodeTypes::Register(constant);

			NodeType arithmetic;
			arithmetic.Id = "number.arithmetic";
			arithmetic.Title = "Arithmetic";
			arithmetic.Category = "Maths";
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
					// Zero rather than an infinity. An infinity propagates into
					// a field, a picture and a saved file, and the first place
					// it is visible is a long way from here.
					result = b == 0.0 ? 0.0 : a / b;
				}
				if (in.Widget("absolute").Flag) {
					result = std::abs(result);
				}
				return Outputs{{"Out", result}};
			};
			NodeTypes::Register(arithmetic);
		}

		void RegisterFieldNodes() {
			// One input, one output, two knobs. The generator every other field
			// case starts from.
			NodeType source;
			source.Id = "field.source";
			source.Title = "Source";
			source.Category = "Generate";
			source.Inputs = {Port("Frequency", "data.NUMBER")};
			source.Outputs = {Port("Out", "data.FIELD")};
			source.Widgets = {
				Select("resolution", "Resolution", {"16", "32", "64", "256"}, 1),
				Slider("frequency", "Frequency", 1.0, 16.0, 4.0),
			};
			source.Evaluate = [](const Inputs &in) {
				// **The wire beats the knob**, which is what makes a connected
				// generator obey its input rather than ignore it.
				const double frequency = in.In<double>("Frequency", in.Real("frequency"));
				return Outputs{{"Out", Make(SideOf(in, "resolution"), frequency)}};
			};
			NodeTypes::Register(source);

			// A second generator with the same shape, so a case can have two
			// independent field sources without them being one hash.
			NodeType ridged = source;
			ridged.Id = "field.ridged";
			ridged.Title = "Ridged";
			NodeTypes::Register(ridged);

			// Two field inputs and one knob. `Maximum` is 0.5 and the label
			// carries "Amount" because the fold case looks the knob up by label
			// and checks the schema survived promotion.
			NodeType warp;
			warp.Id = "field.warp";
			warp.Title = "Warp";
			warp.Category = "Shape";
			warp.Inputs = {Port("In", "data.FIELD"), Port("By", "data.FIELD")};
			warp.Outputs = {Port("Out", "data.FIELD")};
			warp.Widgets = {Slider("amount", "Amount", 0.0, 0.5, 0.2)};
			warp.Evaluate = [](const Inputs &in) {
				Field field = in.In<Field>("In", Make(16, 0.0));
				const double amount = in.Real("amount");
				for (float &sample : field.Samples) {
					sample = static_cast<float>(std::fmod(sample + amount, 1.0));
				}
				return Outputs{{"Out", field}};
			};
			NodeTypes::Register(warp);

			// One field input and two knobs, so folding it with `warp` promotes
			// three.
			NodeType terrace;
			terrace.Id = "field.terrace";
			terrace.Title = "Terrace";
			terrace.Category = "Shape";
			terrace.Inputs = {Port("In", "data.FIELD")};
			terrace.Outputs = {Port("Out", "data.FIELD")};
			terrace.Widgets = {
				Slider("steps", "Steps", 2.0, 16.0, 4.0),
				Toggle("smooth", "Smooth", false),
			};
			terrace.Evaluate = [](const Inputs &in) {
				Field field = in.In<Field>("In", Make(16, 0.0));
				const double steps = std::max(in.Real("steps"), 1.0);
				for (float &sample : field.Samples) {
					sample = static_cast<float>(std::floor(sample * steps) / steps);
				}
				return Outputs{{"Out", field}};
			};
			NodeTypes::Register(terrace);

			// Four inputs, one output, two knobs - the shape the layout case
			// counts. It has a picture because `data.FIELD` does, which is the
			// point: the preview belongs to the wire.
			NodeType blend;
			blend.Id = "field.blend";
			blend.Title = "Blend";
			blend.Category = "Shape";
			blend.Inputs = {
				Port("A", "data.FIELD"),
				Port("B", "data.FIELD"),
				Port("Mask", "data.FIELD"),
				Port("Amount", "data.NUMBER"),
			};
			blend.Outputs = {Port("Out", "data.FIELD")};
			blend.Widgets = {
				Slider("amount", "Amount", 0.0, 1.0, 0.5),
				Select("mode", "Mode", {"mix", "add"}, 0),
			};
			blend.Evaluate = [](const Inputs &in) {
				const Field a = in.In<Field>("A", Make(16, 0.0));
				const Field b = in.In<Field>("B", a);
				const double amount = in.In<double>("Amount", in.Real("amount"));
				Field out = a;
				for (size_t index = 0; index < out.Samples.size() && index < b.Samples.size(); index++) {
					out.Samples[index] =
						static_cast<float>((a.Samples[index] * (1.0 - amount)) + (b.Samples[index] * amount));
				}
				return Outputs{{"Out", out}};
			};
			NodeTypes::Register(blend);

			// Field in, number out. The end of a chain.
			NodeType readout;
			readout.Id = "field.readout";
			readout.Title = "Readout";
			readout.Category = "Read";
			readout.Inputs = {Port("In", "data.FIELD")};
			readout.Outputs = {Port("Out", "data.NUMBER")};
			readout.Evaluate = [](const Inputs &in) {
				const Field field = in.In<Field>("In", Field{});
				double total = 0.0;
				for (const float sample : field.Samples) {
					total += sample;
				}
				return Outputs{
					{"Out", field.Samples.empty() ? 0.0 : total / static_cast<double>(field.Samples.size())}
				};
			};
			NodeTypes::Register(readout);

			// Field in, picture out - a node whose *output* type carries the
			// preview rather than the node.
			NodeType colourise;
			colourise.Id = "image.colourise";
			colourise.Title = "Colourise";
			colourise.Category = "Read";
			colourise.Inputs = {Port("In", "data.FIELD")};
			colourise.Outputs = {Port("Out", "data.IMAGE")};
			colourise.Evaluate = [](const Inputs &in) {
				const Field field = in.In<Field>("In", Make(16, 0.0));
				PreviewImage image;
				FieldPicture(std::any{field}, image);
				for (size_t index = 0; index + 3 < image.Rgba.size(); index += 4) {
					image.Rgba[index + 2] = 255; // blue, so it is not the grey ramp
				}
				return Outputs{{"Out", image}};
			};
			NodeTypes::Register(colourise);
		}

		void RegisterOtherNodes() {
			// **No `Evaluate` at all**, which is what makes it `Skipped` rather
			// than `Waiting`. Its input takes the wildcard, so a case can prove
			// an unregistered-typed drop is refused while `ANY_TYPE` is not.
			NodeType note;
			note.Id = "graph.note";
			note.Title = "Note";
			note.Category = "Layout";
			note.Inputs = {Port("Anything", ANY_TYPE)};
			note.Widgets = {Text("text", "Text", "")};
			NodeTypes::Register(note);

			// The slow one. Sleeps in slices, reports as it goes, and gives up
			// when the run is cancelled.
			NodeType staged;
			staged.Id = "task.staged";
			staged.Title = "Staged Task";
			staged.Category = "Slow";
			staged.Outputs = {Port("Done", "data.NUMBER")};
			staged.Widgets = {
				Number("seconds", "Seconds", 0.2),
				// **In the hash, so two of these are two pieces of work.**
				// Identical nodes are deliberately one - the evaluator makes the
				// second wait on the first - and the overlap case needs two.
				Text("label", "Label", ""),
			};
			staged.Async = true;
			staged.Steps = {"first", "second", "third", "fourth"};
			staged.Evaluate = [](const Inputs &in) {
				const double seconds = std::max(in.Real("seconds"), 0.0);
				const int slices = 20;
				const auto slice = std::chrono::duration<double>(seconds / slices);
				for (int step = 0; step < slices; step++) {
					// Polled every slice, so a shutdown costs one slice rather
					// than the whole duration.
					if (in.Cancelled()) {
						return Outputs{};
					}
					std::this_thread::sleep_for(slice);
					if (in.Report) {
						in.Report(
							static_cast<size_t>(step * 4 / slices),
							static_cast<float>(step + 1) / static_cast<float>(slices),
							"working"
						);
					}
				}
				return Outputs{{"Done", 1.0}};
			};
			NodeTypes::Register(staged);
		}
	}

	void RegisterFixtureNodes() {
		RegisterDataTypes();
		RegisterNumberNodes();
		RegisterFieldNodes();
		RegisterOtherNodes();
	}
}
