// The Demo Nodes panel: a working node system, and the node set it runs.
//
// **A demo and not a feature, said out loud.** Nothing in the engine reads this
// graph — it computes scalar fields and shows them as numbers — and that is the
// point of having it before the two node systems the roadmap wants: the render
// pipeline as a node editor, and `Engine::bakegraph`'s pipeline documents. Both
// need a registry, a cycle guard, a layout and a canvas, and building either
// without having built one is how an editor ends up shaped by whichever came
// first.
//
// **The node set is the smallest one that exercises everything**, rather than a
// terrain library: two data types, every widget kind that can be edited on a
// canvas, a node with two inputs, a node with none, and a node with no
// evaluation at all. Registering one is one call — the palette, the painter, the
// hit test, the evaluator and the save format all read the same table.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <studio/NodeGraph.hpp>
#include <vector>

namespace studio {

	namespace nodes {

		namespace {
			// A square scalar grid. What flows down a wire is the caller's
			// business — `Inputs::In` is the only place that knows — so this
			// type lives here rather than in the library.
			struct Field {
				int Side = 0;
				std::vector<float> Data;
			};

			Field Made(int side) {
				Field field;
				field.Side = side;
				field.Data.assign(static_cast<size_t>(side) * static_cast<size_t>(side), 0.0f);
				return field;
			}

			// Value noise. **Deterministic on purpose**: an `Evaluate` that read
			// a clock or a global would produce a result the cache then refuses
			// to recompute, which is a stale picture that looks like a broken
			// graph.
			float Hashed(int x, int y, int seed) {
				uint32_t value = static_cast<uint32_t>(x) * 374761393u +
								 static_cast<uint32_t>(y) * 668265263u +
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

			// The average of a field, which is what the Output node reports.
			// One number is all a demo panel needs to show that the chain ran.
			float Average(const Field &field) {
				if (field.Data.empty()) {
					return 0.0f;
				}
				double total = 0.0;
				for (const float sample : field.Data) {
					total += static_cast<double>(sample);
				}
				return static_cast<float>(total / static_cast<double>(field.Data.size()));
			}

			bool Registered = false;
		}

		void RegisterDemoNodes() {
			// Idempotent, because the panel registers on first open and the
			// tests register in every case. A second registration replaces in
			// place, so this is a saving rather than a correctness fix — but a
			// palette that grew a duplicate row every open would be neither.
			if (Registered) {
				return;
			}
			Registered = true;

			DataTypes::Register(DataType{"data.NUMBER", "Number", Colour::Hex(0xF2C14E), "One scalar"});
			DataTypes::Register(
				DataType{"data.FIELD", "Field", Colour::Hex(0x4CA6FF), "A square grid of scalars"}
			);

			NodeType constant;
			constant.Id = "number.constant";
			constant.Title = "Number";
			constant.Category = "Input";
			constant.Accent = Colour::Hex(0xF2C14E);
			constant.Outputs = {Port("Out", "data.NUMBER")};
			constant.Widgets = {Number("value", "Value", 1.0)};
			constant.Evaluate = [](const Inputs &in) {
				return Outputs{{"Out", in.Real("value")}};
			};
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
					// Zero rather than an infinity. An infinity propagates into
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

			NodeType noise;
			noise.Id = "field.noise";
			noise.Title = "Noise";
			noise.Category = "Generate";
			noise.Accent = Colour::Hex(0x4CA6FF);
			noise.Subtitle = "value noise, two octaves";
			noise.Inputs = {Port("Scale", "data.NUMBER")};
			noise.Outputs = {Port("Out", "data.FIELD")};
			noise.Widgets = {
				Select("resolution", "Resolution", {"64", "128", "256"}, 1),
				Slider("frequency", "Frequency", 1.0, 16.0, 4.0),
				Number("seed", "Seed", 1.0),
			};
			noise.Evaluate = [](const Inputs &in) {
				const std::string chosen = in.Widget("resolution").Text;
				const int side = chosen.empty() ? 128 : std::stoi(chosen);
				const auto seed = static_cast<int>(in.Real("seed"));

				// **A wire beats the knob.** The knob is what the node does on
				// its own; a wire is somebody saying otherwise, and a node that
				// ignored it would be a wire that does nothing.
				const auto frequency = static_cast<float>(in.In<double>("Scale", in.Real("frequency")));

				Field field = Made(side);
				for (int y = 0; y < side; y++) {
					for (int x = 0; x < side; x++) {
						const float u = static_cast<float>(x) / static_cast<float>(side);
						const float v = static_cast<float>(y) / static_cast<float>(side);
						const float low = Noise(u * frequency, v * frequency, seed);
						const float high = Noise(u * frequency * 2.0f, v * frequency * 2.0f, seed + 1);
						field.Data[static_cast<size_t>(y) * static_cast<size_t>(side) +
								   static_cast<size_t>(x)] = low * 0.7f + high * 0.3f;
					}
				}
				return Outputs{{"Out", field}};
			};
			NodeTypes::Register(noise);

			NodeType combine;
			combine.Id = "field.combine";
			combine.Title = "Combine";
			combine.Category = "Filter";
			combine.Accent = Colour::Hex(0x6FCF97);
			combine.Inputs = {
				Port("A", "data.FIELD"), Port("B", "data.FIELD"), Port("Amount", "data.NUMBER")
			};
			combine.Outputs = {Port("Out", "data.FIELD")};
			combine.Widgets = {Slider("amount", "Amount", 0.0, 1.0, 0.5)};
			combine.Evaluate = [](const Inputs &in) {
				const Field a = in.In<Field>("A");
				const Field b = in.In<Field>("B");
				const auto amount = static_cast<float>(in.In<double>("Amount", in.Real("amount")));

				if (a.Side == 0) {
					return Outputs{{"Out", b}};
				}
				if (b.Side != a.Side) {
					// **Mismatched sizes refuse rather than resample.** Silently
					// stretching one onto the other is a decision the graph
					// should be making with a node somebody can see.
					return Outputs{{"Out", a}};
				}

				Field out = Made(a.Side);
				for (size_t index = 0; index < out.Data.size(); index++) {
					out.Data[index] = a.Data[index] * (1.0f - amount) + b.Data[index] * amount;
				}
				return Outputs{{"Out", out}};
			};
			NodeTypes::Register(combine);

			NodeType readout;
			readout.Id = "field.readout";
			readout.Title = "Readout";
			readout.Category = "Output";
			readout.Accent = Colour::Hex(0xB07AD6);
			readout.Subtitle = "the field's average";
			readout.Inputs = {Port("In", "data.FIELD")};
			readout.Outputs = {Port("Value", "data.NUMBER")};
			readout.Evaluate = [](const Inputs &in) {
				return Outputs{{"Value", static_cast<double>(Average(in.In<Field>("In")))}};
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

			const NodeId first = graph.Add("field.noise", 60.0f, 90.0f);
			const NodeId second = graph.Add("field.noise", 60.0f, 300.0f);
			const NodeId blend = graph.Add("number.constant", 60.0f, 520.0f);
			const NodeId combine = graph.Add("field.combine", 340.0f, 200.0f);
			const NodeId readout = graph.Add("field.readout", 620.0f, 210.0f);
			const NodeId note = graph.Add("graph.note", 620.0f, 340.0f);

			graph.Find(second)->Widgets["seed"].Number = 7.0;
			graph.Find(second)->Widgets["frequency"].Number = 9.0;
			graph.Find(blend)->Widgets["value"].Number = 0.35;

			graph.Connect(first, "Out", combine, "A");
			graph.Connect(second, "Out", combine, "B");
			graph.Connect(blend, "Out", combine, "Amount");
			graph.Connect(combine, "Out", readout, "In");
			graph.Connect(readout, "Value", note, "Anything");
		}
	}

	void Editor::DrawNodeDemo() {
		if (!ShowNodeDemo) {
			return;
		}

		if (!ImGui::Begin("Demo Nodes", &ShowNodeDemo, ImGuiWindowFlags_MenuBar)) {
			ImGui::End();
			return;
		}

		// **Built on the first open rather than at start-up.** A panel nobody
		// opens must cost nothing, which is this program's rule for every panel
		// that answers a question occasionally — and a graph built before the
		// node types were registered would be an empty one.
		if (NodeDemoGraph.Nodes().empty()) {
			nodes::BuildDemoGraph(NodeDemoGraph);
			NodeDemoCanvas.Observe(&NodeDemoRunner);
			NodeDemoSignature = 0;
		}

		if (ImGui::BeginMenuBar()) {
			ImGui::Text(
				"%zu evaluated  %zu cached  %zu skipped",
				NodeDemoReport.Evaluated,
				NodeDemoReport.Cached,
				NodeDemoReport.Skipped
			);

			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			ImGui::TextDisabled("%.0f%%", static_cast<double>(NodeDemoCanvas.Zoom() * 100.0f));

			ImGui::SameLine();
			if (ImGui::SmallButton("Fit")) {
				NodeDemoCanvas.Fit(NodeDemoGraph);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Forget cache")) {
				// What the reference implementation's ⌫ Cache does: the thing to
				// reach for when an `Evaluate` turns out not to be as pure as it
				// claimed.
				NodeDemoRunner.Forget();
				NodeDemoSignature = 0;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Reset graph")) {
				nodes::BuildDemoGraph(NodeDemoGraph);
				NodeDemoRunner.Forget();
				NodeDemoSignature = 0;
			}

			if (!NodeDemoCanvas.LastRefusal.empty()) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
				ImGui::TextUnformatted(NodeDemoCanvas.LastRefusal.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::EndMenuBar();
		}

		// The readouts, above the canvas: what the graph computed, which is the
		// half a picture of boxes cannot show.
		for (const nodes::Node &node : NodeDemoGraph.Nodes()) {
			if (node.Type != "field.readout") {
				continue;
			}
			const std::any *value = NodeDemoRunner.Output(node.Id, "Value");
			if (value == nullptr) {
				continue;
			}
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text(
				"%s = %.4f",
				node.Label.empty() ? "readout" : node.Label.c_str(),
				*std::any_cast<double>(value)
			);
			ImGui::PopStyleColor();
			ImGui::SameLine();
		}
		ImGui::NewLine();

		NodeDemoCanvas.Draw(NodeDemoGraph);

		// **Re-run only when the graph's signature moved.** Dragging a node
		// changes no result, and a graph that recomputed while somebody tidied
		// it up would make a large one unusable — the signature covers
		// parameters and topology and deliberately not position.
		if (const uint64_t now = NodeDemoGraph.Signature(); now != NodeDemoSignature) {
			NodeDemoSignature = now;
			NodeDemoReport = NodeDemoRunner.Run(NodeDemoGraph);
		}

		ImGui::End();
	}

	void Editor::DrawDemoTools() {
		// One demo today, and the row is the list of them. A tab whose contents
		// are a single button is what a second demo turns into a strip with no
		// other change.
		if (ImGui::Button("Demo Nodes", ImVec2(engine::ui::Scaled(110.0f), 0.0f))) {
			ShowNodeDemo = true;
			ImGui::SetWindowFocus("Demo Nodes");
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("A typed node graph with live evaluation — studio/NodeGraph.hpp");
		}

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextUnformatted(
			"demos of engine parts, built to be looked at — nothing here changes the scene"
		);
		ImGui::PopStyleColor();
	}
}
