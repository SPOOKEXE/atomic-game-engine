// The inspector handlers: what a panel draws to answer "what is this node
// doing?" for one selected node.
//
// **One handler per kind of node, picked by name and otherwise inferred**, which
// is `Inspectors::For`'s rule. The alternative — one function with a switch over
// every node type — is a function every new node type has to be added to, and
// the whole point of the registry is that it is not.
//
// ## What a handler draws, and what it does not
//
// A handler draws the *visualisation*: the picture, the stages, the readout.
// Everything around it — the title, the parameters, the port table — is the same
// for every type and is drawn by the panel, so a handler that wanted to be
// different about those would be a handler nobody could predict.
//
// ## The one that matters: inputs beside the output
//
// `field` is the reference implementation's headline panel and the reason
// `DataType::Preview` exists. A node's output is only half the story — the
// question somebody actually has in front of a terrain graph is *what did this
// node do to what it was given*, and that needs both ends drawn from one place
// at one size. The inputs come from upstream nodes of types this handler has
// never heard of, so the picture cannot come from the node type: it comes from
// the wire, which is the one thing both ends agree on.
//
// @tier client

#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <studio/NodeGraph.hpp>
#include <vector>

namespace studio::nodes {

	namespace {

		ImVec4 Vector(const Colour &colour) {
			return ImVec4(colour.R, colour.G, colour.B, colour.A);
		}

		// A data type's colour, for a dot or a label. Grey for a type nobody
		// registered, which is a fact rather than a failure — a saved graph can
		// name one this process has not heard of.
		ImVec4 TintOf(const std::string &type) {
			const DataType *found = DataTypes::Find(type);
			return found != nullptr ? Vector(found->Tint) : ImVec4(0.62f, 0.64f, 0.70f, 1.0f);
		}

		// The small round swatch every port row starts with. Drawn rather than
		// spelled with a glyph, because a bullet's size follows the font and
		// these have to line up down a column.
		void Dot(const std::string &type) {
			const float size = ImGui::GetTextLineHeight();
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddCircleFilled(
				ImVec2(at.x + size * 0.35f, at.y + size * 0.5f),
				size * 0.24f,
				ImGui::GetColorU32(TintOf(type))
			);
			ImGui::Dummy(ImVec2(size * 0.8f, size));
			ImGui::SameLine();
		}

		// One payload the panel can draw, gathered from a port.
		//
		// **`Maker` is whichever node produced it**, not the node being
		// inspected. An input's picture is its upstream's output, so a type that
		// overrode `NodeType::Preview` has to be the one asked about it — or a
		// node would show its neighbour's field through its own colour ramp.
		struct Sampled {
			std::string Port;
			std::string Type;
			const std::any *Payload = nullptr;
			const NodeType *Maker = nullptr;
			uint64_t Key = 0;
		};

		// The type a payload is actually travelling as.
		//
		// An input port declared as the wildcard says nothing about what arrived
		// on it, so the upstream port's type is the useful answer and the
		// declared one is the fallback.
		std::string CarriedType(const NodeType &maker, const std::string &port, const std::string &declared) {
			for (const PortSpec &spec : maker.Outputs) {
				if (spec.Name == port) {
					return spec.Type;
				}
			}
			return declared;
		}

		std::vector<Sampled> Gather(const Inspection &what, bool inputs) {
			std::vector<Sampled> found;
			if (what.Node == nullptr || what.Type == nullptr || what.Runner == nullptr) {
				return found;
			}

			// **The node's own interface, then resolved to where the payload
			// really is.** A compressed node's ports are proxies naming inner
			// ports; asking the evaluator about a proxy by name would find
			// nothing and the panel would say a working fold produced nothing.
			for (const PortSpec &port : inputs ? InputsOf(*what.Node) : OutputsOf(*what.Node)) {
				Sampled one;
				one.Port = port.Name;
				one.Type = port.Type;

				NodeId held = NO_NODE;
				std::string heldPort;
				if (what.Graph == nullptr ||
					!Actual(*what.Graph, what.Node->Id, port.Name, inputs, held, heldPort)) {
					continue;
				}

				if (!inputs) {
					const Node *maker = what.Graph->Find(held);
					one.Payload = what.Runner->Output(held, heldPort);
					one.Maker = maker == nullptr ? nullptr : NodeTypes::Find(maker->Type);
					one.Key = PictureKey(what.Runner->RanAt(held), heldPort);
				} else {
					const Link *link = what.Graph->LinkInto(held, heldPort);
					if (link == nullptr) {
						continue;
					}
					const Node *upstream = what.Graph->Find(link->From);
					const NodeType *maker = upstream == nullptr ? nullptr : NodeTypes::Find(upstream->Type);

					one.Payload = what.Runner->Output(link->From, link->FromPort);
					one.Maker = maker;
					one.Key = PictureKey(what.Runner->RanAt(link->From), link->FromPort);
					if (maker != nullptr) {
						one.Type = CarriedType(*maker, link->FromPort, port.Type);
					}
				}

				if (one.Payload != nullptr) {
					found.push_back(std::move(one));
				}
			}
			return found;
		}

		// The texture for one sample, or null when that payload makes no
		// picture. Converted at most once per result — the sink holds it under
		// `Sampled::Key`.
		void *Picture(const Inspection &what, const Sampled &sample) {
			if (!what.Images || sample.Payload == nullptr) {
				return nullptr;
			}
			const std::any &held = *sample.Payload;
			const NodeType *maker = sample.Maker;
			const std::string type = sample.Type;
			return what.Images(sample.Key, [&held, maker, &type](PreviewImage &image) {
				return PictureOf(maker, type, held, image);
			});
		}

		bool Drawable(const Inspection &what, const Sampled &sample) {
			return Picture(what, sample) != nullptr;
		}

		// Which sample the big view is showing.
		//
		// **In ImGui's per-window storage rather than a static**, so two panels
		// over one graph do not fight over one pinned port — and so it resets
		// with the window rather than living for the process.
		struct Pin {
			int Index = 0;
			NodeId Node = NO_NODE;
		};

		Pin ReadPin() {
			ImGuiStorage *store = ImGui::GetStateStorage();
			Pin pin;
			pin.Index = store->GetInt(ImGui::GetID("nodegraph.pin.index"), 0);
			pin.Node = static_cast<NodeId>(store->GetInt(ImGui::GetID("nodegraph.pin.node"), 0));
			return pin;
		}

		void WritePin(const Pin &pin) {
			ImGuiStorage *store = ImGui::GetStateStorage();
			store->SetInt(ImGui::GetID("nodegraph.pin.index"), pin.Index);
			store->SetInt(ImGui::GetID("nodegraph.pin.node"), static_cast<int>(pin.Node));
		}

		// A section heading, in the panel's quietest voice. Every handler opens
		// its parts with one, so the panel reads as a list of answers rather than
		// a wall.
		void Heading(const char *text) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::SeparatorText(text);
			ImGui::PopStyleColor();
		}

		// A row of thumbnails that wraps, with the port's name under each. The
		// strip is clickable: pressing one moves it into the big view, which is
		// how "show me what went in" is asked without a second panel.
		void Strip(
			const Inspection &what, const char *label, const std::vector<Sampled> &samples, int base, Pin &pin
		) {
			std::vector<size_t> drawable;
			for (size_t index = 0; index < samples.size(); index++) {
				if (Drawable(what, samples[index])) {
					drawable.push_back(index);
				}
			}
			if (drawable.empty()) {
				return;
			}

			Heading(label);

			// Three across where there is room, and never fewer than one — a
			// panel dragged narrow shows a column rather than nothing.
			const float room = ImGui::GetContentRegionAvail().x;
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const auto across =
				static_cast<int>(std::max(1.0f, std::min(3.0f, room / engine::ui::Scaled(74.0f))));
			const float side = (room - spacing * static_cast<float>(across - 1)) / static_cast<float>(across);

			for (size_t at = 0; at < drawable.size(); at++) {
				const Sampled &sample = samples[drawable[at]];
				const int index = base + static_cast<int>(drawable[at]);

				ImGui::BeginGroup();
				ImGui::PushID(index);

				void *handle = Picture(what, sample);
				const bool shown = pin.Index == index;

				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
				ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, shown ? 2.0f : 1.0f);
				ImGui::PushStyleColor(ImGuiCol_Border, TintOf(sample.Type));
				if (ImGui::ImageButton(
						"##thumb", reinterpret_cast<ImTextureID>(handle), ImVec2(side, side)
					)) {
					pin.Index = index;
					WritePin(pin);
				}
				ImGui::PopStyleColor(2);
				ImGui::PopStyleVar();

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"%s — %s", sample.Port.c_str(), DescribeValue(sample.Type, *sample.Payload).c_str()
					);
				}

				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextUnformatted(sample.Port.c_str());
				ImGui::PopStyleColor();

				ImGui::PopID();
				ImGui::EndGroup();

				if (static_cast<int>(at % static_cast<size_t>(across)) != across - 1 &&
					at + 1 < drawable.size()) {
					ImGui::SameLine();
				}
			}
		}

		// --- the handlers -----------------------------------------------------

		// A field or a picture, with whatever went into it beside it.
		void DrawField(const Inspection &what) {
			std::vector<Sampled> outputs = Gather(what, false);
			std::vector<Sampled> inputs = Gather(what, true);

			// One list, so a pin is one index. Outputs first, because the
			// output is what the big view opens on.
			std::vector<Sampled> all = outputs;
			all.insert(all.end(), inputs.begin(), inputs.end());

			Pin pin = ReadPin();
			if (pin.Node != what.Node->Id) {
				// **Reset when the selection moves**, or the third port of one
				// node would be shown as the third port of the next.
				pin.Node = what.Node->Id;
				pin.Index = 0;
				WritePin(pin);
			}

			// The first thing with a picture, which is what the pin falls back
			// to when a port stops producing one mid-edit.
			int primary = -1;
			for (size_t index = 0; index < all.size(); index++) {
				if (Drawable(what, all[index])) {
					primary = static_cast<int>(index);
					break;
				}
			}
			if (primary < 0) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextWrapped("nothing to show yet — this node has not produced a picture");
				ImGui::PopStyleColor();
				return;
			}
			if (pin.Index < 0 || pin.Index >= static_cast<int>(all.size()) ||
				!Drawable(what, all[static_cast<size_t>(pin.Index)])) {
				pin.Index = primary;
				WritePin(pin);
			}

			const Sampled &shown = all[static_cast<size_t>(pin.Index)];

			Heading("visualisation");

			const float side = ImGui::GetContentRegionAvail().x;
			if (void *handle = Picture(what, shown); handle != nullptr) {
				ImGui::Image(reinterpret_cast<ImTextureID>(handle), ImVec2(side, side));
			} else {
				ImGui::Dummy(ImVec2(side, side));
			}

			// The caption, which is the half a picture cannot carry: which port
			// this is, and what it actually contains.
			ImGui::PushStyleColor(ImGuiCol_Text, TintOf(shown.Type));
			ImGui::TextUnformatted(shown.Port.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(DescribeValue(shown.Type, *shown.Payload).c_str());
			ImGui::PopStyleColor();

			Strip(what, "inputs", inputs, static_cast<int>(outputs.size()), pin);
			Strip(what, "outputs", outputs, 0, pin);
		}

		// An async node: the run is the interesting thing, so the stages are.
		void DrawRun(const Inspection &what) {
			const NodeStatus status = what.Runner->Status(what.Node->Id);

			Heading("run");

			const size_t done = status.State == NodeState::Done ? what.Type->Steps.size() : status.Step;
			const char *state = status.State == NodeState::Running	? "running"
								: status.State == NodeState::Done	? "complete"
								: status.State == NodeState::Failed ? "failed"
																	: "queued";

			ImGui::PushStyleColor(
				ImGuiCol_Text,
				status.State == NodeState::Failed ? engine::ui::WarningColour()
				: status.State == NodeState::Idle ? engine::ui::MutedColour()
												  : engine::ui::AccentColour()
			);
			ImGui::TextUnformatted(state);
			ImGui::PopStyleColor();

			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			if (status.Cached) {
				ImGui::Text("· %zu/%zu steps · cached", done, what.Type->Steps.size());
			} else if (status.State == NodeState::Done) {
				ImGui::Text("· %zu/%zu steps · %.0f ms", done, what.Type->Steps.size(), status.Milliseconds);
			} else {
				ImGui::Text("· %zu/%zu steps", done, what.Type->Steps.size());
			}
			ImGui::PopStyleColor();

			ImGui::ProgressBar(
				std::clamp(status.Progress, 0.0f, 1.0f), ImVec2(-1.0f, engine::ui::Scaled(6.0f)), ""
			);

			// The stages, with the one it is on marked. A bar says how far; this
			// says through what, which is what a percentage cannot carry.
			for (size_t index = 0; index < what.Type->Steps.size(); index++) {
				const bool current = status.State == NodeState::Running && index == status.Step;
				const bool passed = status.State == NodeState::Done || index < status.Step;

				// **One type through the ternary.** The theme hands back packed
				// colours and `GetStyleColorVec4` hands back a vector; mixing
				// them in one expression is a conversion the compiler refuses,
				// which is the good version of that mistake.
				ImGui::PushStyleColor(
					ImGuiCol_Text,
					current	 ? engine::ui::AccentColour()
					: passed ? ImGui::GetColorU32(ImGuiCol_Text)
							 : engine::ui::MutedColour()
				);
				ImGui::Text(
					"%s %s",
					current	 ? ">"
					: passed ? "done"
							 : "    ",
					what.Type->Steps[index].c_str()
				);
				ImGui::PopStyleColor();

				if (current && !status.Note.empty()) {
					ImGui::SameLine();
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
					ImGui::TextUnformatted(status.Note.c_str());
					ImGui::PopStyleColor();
				}
			}

			// And whatever it produced, since a staged node usually ends in one
			// small value rather than a picture.
			const std::vector<Sampled> outputs = Gather(what, false);
			if (outputs.empty()) {
				return;
			}
			Heading("result");
			for (const Sampled &sample : outputs) {
				Dot(sample.Type);
				ImGui::TextUnformatted(sample.Port.c_str());
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextUnformatted(DescribeValue(sample.Type, *sample.Payload).c_str());
				ImGui::PopStyleColor();
			}
		}

		// A node whose output is data rather than a picture.
		void DrawValue(const Inspection &what) {
			Heading("output");

			const std::vector<Sampled> outputs = Gather(what, false);
			const std::vector<PortSpec> declared = OutputsOf(*what.Node);
			if (declared.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextWrapped("this node is a sink — it has no outputs");
				ImGui::PopStyleColor();
			}

			for (const PortSpec &port : declared) {
				const auto found = std::find_if(outputs.begin(), outputs.end(), [&](const Sampled &one) {
					return one.Port == port.Name;
				});

				Dot(port.Type);
				ImGui::TextUnformatted(port.Name.c_str());
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextWrapped(
					"%s",
					found == outputs.end() ? "not evaluated"
										   : DescribeValue(found->Type, *found->Payload).c_str()
				);
				ImGui::PopStyleColor();
			}

			// What it was given, on the same terms. A readout with no inputs
			// listed is a number with no explanation of where it came from.
			const std::vector<Sampled> inputs = Gather(what, true);
			if (inputs.empty()) {
				return;
			}
			Heading("inputs");
			for (const Sampled &sample : inputs) {
				Dot(sample.Type);
				ImGui::TextUnformatted(sample.Port.c_str());
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextWrapped("%s", DescribeValue(sample.Type, *sample.Payload).c_str());
				ImGui::PopStyleColor();
			}
		}

		// Nothing has been computed. **Which of the three reasons matters**, and
		// they need different answers: a node with no evaluation never will
		// produce anything, one whose graph has not been built will, and a sink
		// was never going to.
		void DrawEmpty(const Inspection &what) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());

			const std::vector<PortSpec> declared =
				what.Node == nullptr ? std::vector<PortSpec>{} : OutputsOf(*what.Node);

			if (what.Type == nullptr) {
				ImGui::TextWrapped("no node type of that name is registered, so there is nothing to run");
			} else if (declared.empty()) {
				ImGui::TextWrapped("this node is a sink — it is somewhere a wire ends and nothing more");
			} else if (!what.Type->Evaluate) {
				ImGui::TextWrapped(
					"this type carries parameters only: nothing is bound to NodeType::Evaluate, so "
					"nothing is computed. It would produce:"
				);
			} else {
				ImGui::TextWrapped("nothing computed yet — press Build. It will produce:");
			}
			ImGui::PopStyleColor();

			if (what.Type == nullptr) {
				return;
			}
			for (const PortSpec &port : declared) {
				Dot(port.Type);
				ImGui::TextUnformatted(port.Name.c_str());
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextUnformatted(port.Type.c_str());
				ImGui::PopStyleColor();
			}
		}

		bool Registered = false;
	}

	void RegisterInspectors() {
		// Idempotent, and for `RegisterDemoNodes`' reason: `Inspectors::For`
		// calls this on every lookup so that a host which registered a node type
		// and nothing else still gets panels.
		if (Registered) {
			return;
		}
		Registered = true;

		Inspectors::Register("field", DrawField);
		Inspectors::Register("run", DrawRun);
		Inspectors::Register("value", DrawValue);
		Inspectors::Register("empty", DrawEmpty);
	}
}
