// The node graph, and the canvas over it.
//
// **Two halves, and only one of them needs a frame.** The model — link
// validation, the cycle guard, the content hash, the save format — is where a
// mistake is silent: a cycle is a hang, a hash that never settles recomputes the
// graph for ever, and a dropped widget is somebody's work lost. Those are
// ordinary functions and most of this file.
//
// The canvas gets a frame, for `studio.assetrow`'s reason: an imgui context is
// not a device, so a window can be submitted, a mouse can be moved and clicked,
// and everything that is not a rasteriser answers. What is checked there is not
// how it looks — it is that a click on a port makes a link and a click on empty
// space does not, which is the half that would otherwise only be found by
// somebody dragging in the editor.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <imgui.h>
#include <string>
#include <studio/NodeGraph.hpp>
#include <thread>
#include <vector>

TEST_SUITE_ID("studio.nodegraph")

using namespace studio::nodes;

namespace {
	// The demo's node types, registered once per process. `RegisterDemoNodes`
	// is idempotent, so every case may ask.
	void Types() {
		RegisterDemoNodes();
	}

	// A bare imgui context around one case — `studio.assetrow` carries why this
	// is per case and not shared.
	class Context {
	  public:
		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();

			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};

	constexpr float WINDOW_X = 20.0f;
	constexpr float WINDOW_Y = 20.0f;

	// Submits one frame with the canvas filling a window at a known place, with
	// the pointer where a case put it.
	void Frame(Canvas &canvas, Graph &graph, float mouseX, float mouseY, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouseX, mouseY);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);

		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(WINDOW_X, WINDOW_Y));
		ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f));
		if (ImGui::Begin(
				"canvas",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoScrollbar
			)) {
			canvas.Draw(graph);
		}
		ImGui::End();
		ImGui::Render();
	}

	// Where a port is on screen, given the canvas has not been panned or zoomed
	// and the window's content starts at its padding.
	void ScreenPort(const Node &node, const std::string &name, bool input, float &x, float &y) {
		const NodeLayout layout = LayoutOf(node);
		const PlacedPort *port = PortIn(layout, name, input);
		REQUIRE(port != nullptr);

		const ImVec2 padding = ImGui::GetStyle().WindowPadding;
		x = WINDOW_X + padding.x + node.X + port->X;
		y = WINDOW_Y + padding.y + node.Y + port->Y;
	}
}

// --- the model ----------------------------------------------------------------

TEST_CASE("a type id is the compatibility rule", "[studio][nodegraph]") {
	Types();

	CHECK(DataTypes::CanConnect("data.FIELD", "data.FIELD"));
	CHECK_FALSE(DataTypes::CanConnect("data.FIELD", "data.NUMBER"));
	CHECK(DataTypes::CanConnect("data.FIELD", ANY_TYPE));

	// **An unregistered id is not a wildcard.** A typo in a port's type would
	// otherwise connect to everything, which is the worst possible reading of a
	// mistake.
	CHECK_FALSE(DataTypes::CanConnect("data.TYPO", "data.FIELD"));
	CHECK_FALSE(DataTypes::CanConnect("", "data.FIELD"));
}

TEST_CASE("a link is refused for a specific reason", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	const NodeId combine = graph.Add("field.combine", 260.0f, 0.0f);
	const NodeId number = graph.Add("number.constant", 0.0f, 240.0f);
	REQUIRE(noise != NO_NODE);

	CHECK(graph.Connect(noise, "Out", combine, "A") == LinkResult::Made);
	CHECK(graph.Connect(number, "Out", combine, "A") == LinkResult::TypeMismatch);
	CHECK(graph.Connect(number, "Out", combine, "Amount") == LinkResult::Made);
	CHECK(graph.Connect(noise, "Out", combine, "Nope") == LinkResult::NoSuchPort);
	CHECK(graph.Connect(noise, "Out", noise, "Frequency") == LinkResult::SameNode);

	// An input takes one link and the newer one wins.
	const NodeId second = graph.Add("field.perlin", 0.0f, 480.0f);
	CHECK(graph.Connect(second, "Out", combine, "A") == LinkResult::Made);
	REQUIRE(graph.LinkInto(combine, "A") != nullptr);
	CHECK(graph.LinkInto(combine, "A")->From == second);
	CHECK(graph.Links().size() == 2);

	// An unregistered type is refused rather than placed as a mystery.
	CHECK(graph.Add("nobody.registered.this", 0.0f, 0.0f) == NO_NODE);
}

TEST_CASE("a cycle is refused before it exists", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId one = graph.Add("field.combine", 0.0f, 0.0f);
	const NodeId two = graph.Add("field.combine", 200.0f, 0.0f);
	const NodeId three = graph.Add("field.combine", 400.0f, 0.0f);

	CHECK(graph.Connect(one, "Out", two, "A") == LinkResult::Made);
	CHECK(graph.Connect(two, "Out", three, "A") == LinkResult::Made);

	// **The long way round is the one a shallow check misses**, and a cycle
	// that got in would be a hang inside `Ordered` rather than a wrong picture.
	CHECK(graph.Connect(three, "Out", one, "A") == LinkResult::WouldCycle);
	CHECK(graph.Connect(two, "Out", one, "A") == LinkResult::WouldCycle);
	CHECK(graph.Links().size() == 2);

	// Removing a node takes its links with it, or every later walk has to guard
	// against an endpoint that is not there.
	CHECK(graph.Remove(two));
	CHECK(graph.Links().empty());
}

TEST_CASE("a hash covers parameters and inputs and nothing else", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	const NodeId combine = graph.Add("field.combine", 200.0f, 0.0f);
	REQUIRE(graph.Connect(noise, "Out", combine, "A") == LinkResult::Made);

	const uint64_t before = graph.Hash(combine);

	// Moving or renaming a node must invalidate nothing: a hash that included
	// position would recompute the whole graph every time somebody tidied it up.
	graph.Find(combine)->X += 40.0f;
	graph.Find(combine)->Label = "renamed";
	CHECK(graph.Hash(combine) == before);

	// An upstream edit invalidates downstream.
	graph.Find(noise)->Widgets["frequency"].Number = 9.0;
	CHECK(graph.Hash(combine) != before);

	// A sideways edit does not touch a node that does not read it.
	const NodeId lonely = graph.Add("field.perlin", 0.0f, 400.0f);
	const uint64_t lonelyBefore = graph.Hash(lonely);
	graph.Find(combine)->Widgets["amount"].Number = 0.25;
	CHECK(graph.Hash(lonely) == lonelyBefore);

	// **It settles.** Asking twice with nothing changed has to answer the same,
	// or every cache lookup misses for ever — the failure the reference
	// implementation hit by hashing evaluation state into it.
	CHECK(graph.Hash(combine) == graph.Hash(combine));
	CHECK(graph.Signature() == graph.Signature());
}

TEST_CASE("evaluation runs in order and caches what has not changed", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId a = graph.Add("number.constant", 0.0f, 0.0f);
	const NodeId b = graph.Add("number.constant", 0.0f, 120.0f);
	const NodeId sum = graph.Add("number.arithmetic", 220.0f, 0.0f);

	graph.Find(a)->Widgets["value"].Number = 2.0;
	graph.Find(b)->Widgets["value"].Number = 5.0;
	REQUIRE(graph.Connect(a, "Out", sum, "A") == LinkResult::Made);
	REQUIRE(graph.Connect(b, "Out", sum, "B") == LinkResult::Made);

	Evaluator runner;
	RunReport report = runner.Run(graph);
	CHECK(report.Evaluated == 3);
	CHECK(report.Cached == 0);

	const std::any *result = runner.Output(sum, "Out");
	REQUIRE(result != nullptr);
	CHECK(std::any_cast<double>(*result) == 7.0);

	// Nothing changed: everything is a hit.
	report = runner.Run(graph);
	CHECK(report.Evaluated == 0);
	CHECK(report.Cached == 3);
	CHECK(runner.WasCached(sum));

	// One edit invalidates exactly what reads it.
	graph.Find(a)->Widgets["value"].Number = 10.0;
	report = runner.Run(graph);
	CHECK(report.Evaluated == 2);
	CHECK(report.Cached == 1);
	CHECK(std::any_cast<double>(*runner.Output(sum, "Out")) == 15.0);

	// **Putting it back is a hit rather than a recompute**, which is the whole
	// reason results are keyed by hash and not by node.
	graph.Find(a)->Widgets["value"].Number = 2.0;
	report = runner.Run(graph);
	CHECK(report.Evaluated == 0);
	CHECK(report.Cached == 3);

	runner.Forget();
	CHECK(runner.Held() == 0);
}

TEST_CASE("a wire beats a knob, and a node with no eval is skipped", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId scale = graph.Add("number.constant", 0.0f, 0.0f);
	const NodeId noise = graph.Add("field.perlin", 200.0f, 0.0f);
	const NodeId note = graph.Add("graph.note", 400.0f, 0.0f);

	graph.Find(scale)->Widgets["value"].Number = 12.0;
	graph.Find(noise)->Widgets["frequency"].Number = 2.0;
	REQUIRE(graph.Connect(scale, "Out", noise, "Frequency") == LinkResult::Made);

	// The wildcard input takes a number even though nothing declares one.
	CHECK(graph.Connect(noise, "Out", note, "Anything") == LinkResult::Made);

	Evaluator runner;
	const RunReport report = runner.Run(graph);
	CHECK(report.Evaluated == 2);
	CHECK(report.Skipped == 1);

	// The connected value, not the knob: a graph where the wire did nothing
	// would be one where every generator ignored its own inputs.
	const uint64_t wired = graph.Hash(noise);
	graph.Find(scale)->Widgets["value"].Number = 3.0;
	CHECK(graph.Hash(noise) != wired);
}

TEST_CASE("layout puts every port and widget inside the node", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId combine = graph.Add("field.combine", 0.0f, 0.0f);
	const NodeLayout layout = LayoutOf(*graph.Find(combine));

	// Four inputs and one output, and two knobs.
	CHECK(layout.Ports.size() == 5);
	CHECK(layout.Widgets.size() == 2);

	// It has a picture, so the layout reserved a square for one.
	CHECK(layout.PreviewSide > 0.0f);
	CHECK(layout.PreviewTop > 0.0f);
	CHECK(layout.PreviewTop + layout.PreviewSide <= layout.Height);

	for (const PlacedPort &port : layout.Ports) {
		CHECK(port.Y > 0.0f);
		CHECK(port.Y < layout.Height);
		CHECK(port.X == (port.Input ? 0.0f : layout.Width));
	}
	for (const PlacedWidget &widget : layout.Widgets) {
		CHECK(widget.Y >= layout.WidgetsTop);
		CHECK(widget.Y + widget.Height <= layout.Height);
		CHECK(widget.X + widget.Width <= layout.Width);
	}

	CHECK(PortIn(layout, "A", true) != nullptr);
	CHECK(PortIn(layout, "A", false) == nullptr);

	// A node whose type is not registered still has a body, so it can be seen,
	// moved and deleted rather than being invisible.
	Node stranger;
	stranger.Type = "somebody.plugin";
	const NodeLayout broken = LayoutOf(stranger);
	CHECK(broken.Width > 0.0f);
	CHECK(broken.Height > 0.0f);
	CHECK(broken.Ports.empty());
}

TEST_CASE("a graph survives a save and a load", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 40.0f, 60.0f);
	const NodeId combine = graph.Add("field.combine", 300.0f, 60.0f);
	graph.Find(noise)->Widgets["frequency"].Number = 7.5;
	graph.Find(noise)->Widgets["resolution"].Text = "256";
	graph.Find(combine)->Label = "to disk";
	REQUIRE(graph.Connect(noise, "Out", combine, "A") == LinkResult::Made);

	const std::string text = Save(graph);
	const uint64_t before = graph.Signature();

	Graph loaded;
	std::string error;
	REQUIRE(Load(text, loaded, error));
	CHECK(error.empty());
	CHECK(loaded.Nodes().size() == 2);
	CHECK(loaded.Links().size() == 1);

	// **The signature rather than a field-by-field compare.** It is what the
	// cache trusts, so a load that changed anything the evaluator can see fails
	// here — and one that changed only a position does not, which is correct.
	CHECK(loaded.Signature() == before);
	CHECK(Save(loaded) == text);
}

TEST_CASE("frames hold members, and neither they nor collapsing change a hash", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 40.0f, 60.0f);
	const NodeId ridge = graph.Add("field.ridged", 40.0f, 340.0f);
	const NodeId blend = graph.Add("field.combine", 300.0f, 60.0f);
	REQUIRE(graph.Connect(noise, "Out", blend, "A") == LinkResult::Made);
	REQUIRE(graph.Connect(ridge, "Out", blend, "B") == LinkResult::Made);

	const uint64_t settled = graph.Signature();

	// **Neither is a parameter.** A frame is where somebody put a rectangle and
	// collapsing is a node somebody has finished reading; if either reached the
	// hash, tidying a graph would recompute it.
	const GroupId frame = graph.Group({noise, ridge}, "Sources", Colour::Hex(0x4ADE80));
	REQUIRE(frame != NO_GROUP);
	graph.Find(blend)->Collapsed = true;
	CHECK(graph.Signature() == settled);

	CHECK(graph.GroupOf(noise) == frame);
	CHECK(graph.GroupOf(blend) == NO_GROUP);

	// A collapsed node keeps every port and loses its body, because a graph that
	// stopped being readable when it was tidied would be the opposite of tidy.
	const NodeLayout shut = LayoutOf(*graph.Find(blend));
	const NodeLayout open = LayoutOf(*graph.Find(noise));
	CHECK(shut.Ports.size() == 5);
	CHECK(shut.Widgets.empty());
	CHECK(shut.PreviewSide == 0.0f);
	CHECK(shut.Height < open.Height);

	// A node joining a second frame leaves the first, so one drag never moves it
	// twice.
	const GroupId second = graph.Group({ridge, blend}, "Rest", Colour::Hex(0x38BDF8));
	CHECK(graph.GroupOf(ridge) == second);
	CHECK(graph.FindGroup(frame)->Members.size() == 1);

	const std::string text = Save(graph);
	Graph loaded;
	std::string error;
	REQUIRE(Load(text, loaded, error));
	CHECK(loaded.Groups().size() == 2);
	CHECK(loaded.Signature() == settled);
	CHECK(Save(loaded) == text);

	// Removing a node takes it out of its frame, so no member id ever names
	// something that is not there.
	REQUIRE(loaded.Remove(loaded.Nodes().front().Id));
	for (const Group &held : loaded.Groups()) {
		for (const NodeId member : held.Members) {
			CHECK(loaded.Alive(member));
		}
	}
}

TEST_CASE("compression derives an interface and moves nothing", "[studio][nodegraph]") {
	Types();
	Graph graph;

	//   noise ──▶ warp ──▶ terrace ──▶ readout
	//   ridged ─▶ warp(By)
	// Folding warp and terrace should take one input from each side of the pair
	// and give one output, with every wire exactly where it was.
	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	const NodeId ridged = graph.Add("field.ridged", 0.0f, 300.0f);
	const NodeId warp = graph.Add("field.warp", 300.0f, 0.0f);
	const NodeId terrace = graph.Add("field.terrace", 600.0f, 0.0f);
	const NodeId average = graph.Add("field.readout", 900.0f, 0.0f);
	graph.Find(noise)->Widgets["resolution"].Text = "32";
	graph.Find(ridged)->Widgets["resolution"].Text = "32";

	REQUIRE(graph.Connect(noise, "Out", warp, "In") == LinkResult::Made);
	REQUIRE(graph.Connect(ridged, "Out", warp, "By") == LinkResult::Made);
	REQUIRE(graph.Connect(warp, "Out", terrace, "In") == LinkResult::Made);
	REQUIRE(graph.Connect(terrace, "Out", average, "In") == LinkResult::Made);

	const size_t wires = graph.Links().size();
	std::vector<std::pair<NodeId, uint64_t>> hashes;
	for (const Node &one : graph.Nodes()) {
		hashes.emplace_back(one.Id, graph.Hash(one.Id));
	}

	const NodeId folded = graph.Compress({warp, terrace}, 450.0f, 0.0f);
	REQUIRE(folded != NO_NODE);

	// **No link was re-pointed and no content hash moved.** That is the whole
	// design: the evaluator, the cycle guard and the cache never learn that
	// compression happened, so none of them can be wrong about it — and folding
	// a chain therefore recomputes nothing.
	CHECK(graph.Links().size() == wires);
	for (const auto &[id, was] : hashes) {
		CHECK(graph.Hash(id) == was);
	}
	CHECK(graph.Contents(folded).size() == 2);
	CHECK(graph.Find(warp)->Owner == folded);
	CHECK(graph.Find(noise)->Owner == NO_NODE);

	// Two inbound target ports, one outbound source port. Types are inherited
	// from the inner ports, so typing survives the fold.
	const Node &node = *graph.Find(folded);
	CHECK(InputsOf(node).size() == 2);
	REQUIRE(OutputsOf(node).size() == 1);
	CHECK(OutputsOf(node).front().Type == "data.FIELD");

	// Every inner knob is promoted, keeping its schema — a slider stays a
	// slider rather than becoming a number box holding the same value.
	const std::vector<WidgetSpec> promoted = WidgetsOf(node);
	CHECK(promoted.size() == 3);
	const auto amount = std::find_if(promoted.begin(), promoted.end(), [](const WidgetSpec &spec) {
		return spec.Label.find("Amount") != std::string::npos;
	});
	REQUIRE(amount != promoted.end());
	CHECK(amount->Kind == WidgetKind::Slider);
	CHECK(amount->Maximum == 0.5);

	// **A write to a promoted knob is a write to the node inside**, which is
	// what makes a compressed node a live view of its contents rather than a
	// copy that drifts from them.
	Value moved = ValueOf(graph, folded, *amount);
	moved.Number = 0.4;
	SetValue(graph, folded, amount->Key, moved);
	CHECK(graph.Find(warp)->Widgets["amount"].Number == 0.4);
	CHECK(graph.Hash(warp) != hashes[2].second);

	// A wire crossing into the fold is drawn to its proxy port; one wholly
	// inside it is not drawn at the root at all.
	NodeId shown = NO_NODE;
	std::string shownPort;
	REQUIRE(Standing(graph, warp, "In", true, NO_NODE, shown, shownPort));
	CHECK(shown == folded);
	NodeId inner = NO_NODE;
	std::string innerPort;
	REQUIRE(Actual(graph, folded, shownPort, true, inner, innerPort));
	CHECK(inner == warp);
	CHECK(innerPort == "In");

	// Seen from inside, the members are themselves again.
	REQUIRE(Standing(graph, warp, "In", true, folded, shown, shownPort));
	CHECK(shown == warp);

	// Save, load, and it is all still true — including the promoted schema,
	// which is re-derived from the inner type rather than written out.
	const std::string text = Save(graph);
	Graph loaded;
	std::string error;
	REQUIRE(Load(text, loaded, error));
	CHECK(Save(loaded) == text);

	const auto sameShape = [&](const Graph &other) {
		for (const Node &one : other.Nodes()) {
			if (one.Compressed()) {
				return InputsOf(one).size() == 2 && OutputsOf(one).size() == 1 && WidgetsOf(one).size() == 3;
			}
		}
		return false;
	};
	CHECK(sameShape(loaded));

	// **A fold copies with everything inside it.** A copy whose proxies still
	// named the original's members would read as a duplicate and behave as a
	// second view of the first, which is the worst of both.
	{
		Canvas canvas;
		canvas.Select(folded);
		canvas.Copy(graph);
		canvas.Paste(graph);

		REQUIRE(canvas.Selection().size() == 1);
		const NodeId copy = canvas.Selection().front();
		CHECK(copy != folded);

		const std::vector<NodeId> inside = graph.Contents(copy);
		CHECK(inside.size() == 2);
		for (const Proxy &proxy : graph.Find(copy)->Proxies) {
			CHECK(std::find(inside.begin(), inside.end(), proxy.Inner) != inside.end());
		}

		// And its promoted knobs write into its own contents rather than the
		// original's — with keys naming the copy's members, not the original's.
		const std::vector<WidgetSpec> knobs = WidgetsOf(*graph.Find(copy));
		REQUIRE(knobs.size() == 3);
		const auto mine = std::find_if(knobs.begin(), knobs.end(), [&](const WidgetSpec &spec) {
			return spec.Label.find("Amount") != std::string::npos;
		});
		REQUIRE(mine != knobs.end());

		Value nudged = ValueOf(graph, copy, *mine);
		nudged.Number = 0.123;
		SetValue(graph, copy, mine->Key, nudged);
		CHECK(graph.Find(warp)->Widgets["amount"].Number == 0.4);
		CHECK(mine->Key.rfind(std::to_string(inside.front()) + "/", 0) == 0);

		REQUIRE(graph.Remove(copy));
	}

	// Expanding is the exact inverse.
	REQUIRE(graph.Expand(folded));
	CHECK(graph.Find(warp)->Owner == NO_NODE);
	CHECK(graph.Links().size() == wires);
	CHECK_FALSE(graph.Alive(folded));

	// And deleting a fold takes its contents, so nothing is left that no view
	// can reach.
	const NodeId again = graph.Compress({warp, terrace}, 450.0f, 0.0f);
	REQUIRE(again != NO_NODE);
	REQUIRE(graph.Remove(again));
	CHECK_FALSE(graph.Alive(warp));
	CHECK_FALSE(graph.Alive(terrace));
	CHECK(graph.Alive(noise));
}

TEST_CASE("a fold files as a library type and places independent copies", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	const NodeId warp = graph.Add("field.warp", 300.0f, 0.0f);
	const NodeId terrace = graph.Add("field.terrace", 600.0f, 0.0f);
	REQUIRE(graph.Connect(noise, "Out", warp, "In") == LinkResult::Made);
	REQUIRE(graph.Connect(warp, "Out", terrace, "In") == LinkResult::Made);

	const NodeId folded = graph.Compress({warp, terrace}, 450.0f, 0.0f);
	REQUIRE(folded != NO_NODE);
	graph.Find(warp)->Widgets["amount"].Number = 0.33;

	graph.Remember("Erosion Stack", SaveSubtree(graph, folded));
	REQUIRE(graph.Templates().size() == 1);

	// **The template is a whole document**, so it opens on its own — which is
	// what makes it hand-editable and what makes placing it one `Load`.
	Graph opened;
	std::string error;
	REQUIRE(Load(graph.Templates().front().Document, opened, error));
	CHECK(opened.Nodes().size() == 3);

	// The subtree comes out at the origin, so placing it is an offset rather
	// than a subtraction every caller has to remember.
	const Node *root = nullptr;
	for (const Node &one : opened.Nodes()) {
		if (one.Compressed()) {
			root = &one;
		}
	}
	REQUIRE(root != nullptr);
	CHECK(root->X == 0.0f);
	CHECK(root->Y == 0.0f);

	// Two placements, each independent of the original and of each other.
	const std::vector<NodeId> first = graph.Absorb(opened, 1000.0f, 0.0f);
	const std::vector<NodeId> second = graph.Absorb(opened, 1000.0f, 400.0f);
	REQUIRE(first.size() == 1);
	REQUIRE(second.size() == 1);
	CHECK(first.front() != second.front());

	const std::vector<WidgetSpec> knobs = WidgetsOf(*graph.Find(first.front()));
	const auto amount = std::find_if(knobs.begin(), knobs.end(), [](const WidgetSpec &spec) {
		return spec.Label.find("Amount") != std::string::npos;
	});
	REQUIRE(amount != knobs.end());

	// The copy carried the value it was filed with, and moving it moves nothing
	// else.
	CHECK(ValueOf(graph, first.front(), *amount).Number == 0.33);
	Value moved = ValueOf(graph, first.front(), *amount);
	moved.Number = 0.11;
	SetValue(graph, first.front(), amount->Key, moved);
	CHECK(graph.Find(warp)->Widgets["amount"].Number == 0.33);
	CHECK(ValueOf(graph, second.front(), *knobs.begin()).Kind == knobs.begin()->Kind);

	// **The library rides in the document**, so a saved graph is the whole
	// thing — the failure the reference implementation has by keeping its custom
	// types in browser storage.
	const std::string text = Save(graph);
	Graph reloaded;
	REQUIRE(Load(text, reloaded, error));
	REQUIRE(reloaded.Templates().size() == 1);
	CHECK(reloaded.Templates().front().Name == "Erosion Stack");
	CHECK(Save(reloaded) == text);

	CHECK(graph.Forget("Erosion Stack"));
	CHECK(graph.Templates().empty());
	CHECK_FALSE(graph.Forget("Erosion Stack"));
}

TEST_CASE("a height field draws as a lit surface", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	graph.Find(noise)->Widgets["resolution"].Text = "64";

	Evaluator runner;
	runner.Run(graph);
	const std::any *payload = runner.Output(noise, "Out");
	REQUIRE(payload != nullptr);

	// **The wire says how to be read as elevation**, the same way it says how to
	// be drawn — so a 3-D view needs nothing from the node type.
	Surface surface;
	REQUIRE(SurfaceOf("data.FIELD", *payload, surface));
	CHECK(surface.Side == 64);
	CHECK_FALSE(SurfaceOf("data.NUMBER", std::any(0.5), surface));

	PreviewImage picture;
	REQUIRE(RenderSurface(surface, nullptr, 0.6f, 0.62f, 0.42f, 96, picture));
	REQUIRE(picture.Valid());
	CHECK(picture.Side == 96);

	// Something was drawn: a picture that is all background is a rasteriser that
	// projected everything off screen, which is the failure this catches.
	size_t painted = 0;
	for (size_t at = 0; at + 3 < picture.Rgba.size(); at += 4) {
		painted += picture.Rgba[at] > 20 || picture.Rgba[at + 1] > 20 ? 1 : 0;
	}
	CHECK(painted > picture.Side * 4);

	// Turning the camera changes the picture, and turning it back gives the same
	// one — which is what lets the view be cached on its angle.
	PreviewImage turned;
	REQUIRE(RenderSurface(surface, nullptr, 1.9f, 0.62f, 0.42f, 96, turned));
	CHECK(turned.Rgba != picture.Rgba);

	PreviewImage again;
	REQUIRE(RenderSurface(surface, nullptr, 0.6f, 0.62f, 0.42f, 96, again));
	CHECK(again.Rgba == picture.Rgba);

	// A flat surface still draws, and a degenerate one is refused rather than
	// rasterised into a divide by zero.
	Surface flat;
	flat.Side = 4;
	flat.Heights.assign(16, 0.5f);
	CHECK(RenderSurface(flat, nullptr, 0.0f, 0.6f, 0.42f, 64, picture));
	CHECK_FALSE(RenderSurface(Surface{}, nullptr, 0.0f, 0.6f, 0.42f, 64, picture));
}

TEST_CASE("an exported picture is a PNG a decoder would accept", "[studio][nodegraph]") {
	// A gradient, so a bug that wrote one row everywhere still fails.
	PreviewImage image;
	image.Side = 37;
	image.Rgba.assign(static_cast<size_t>(image.Side) * image.Side * 4, 0);
	for (uint32_t y = 0; y < image.Side; y++) {
		for (uint32_t x = 0; x < image.Side; x++) {
			const size_t at = (static_cast<size_t>(y) * image.Side + x) * 4;
			image.Rgba[at] = static_cast<uint8_t>(x * 7);
			image.Rgba[at + 1] = static_cast<uint8_t>(y * 5);
			image.Rgba[at + 2] = static_cast<uint8_t>(x + y);
			image.Rgba[at + 3] = 255;
		}
	}

	const std::vector<uint8_t> file = EncodePng(image);
	REQUIRE(file.size() > 8);

	const std::vector<uint8_t> signature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	CHECK(std::equal(signature.begin(), signature.end(), file.begin()));

	// **Every chunk's CRC is checked**, because a wrong one is the failure mode
	// here: the file opens in nothing and says nothing about why. The CRC covers
	// the type and the payload and *not* the length, which is the part that is
	// easy to get wrong and impossible to notice without a reader.
	const auto crc = [](const uint8_t *bytes, size_t count) {
		uint32_t running = 0xFFFFFFFFu;
		for (size_t at = 0; at < count; at++) {
			running ^= bytes[at];
			for (int bit = 0; bit < 8; bit++) {
				running = (running & 1u) != 0u ? 0xEDB88320u ^ (running >> 1) : running >> 1;
			}
		}
		return running ^ 0xFFFFFFFFu;
	};
	const auto read32 = [&file](size_t at) {
		return (static_cast<uint32_t>(file[at]) << 24) | (static_cast<uint32_t>(file[at + 1]) << 16) |
			   (static_cast<uint32_t>(file[at + 2]) << 8) | static_cast<uint32_t>(file[at + 3]);
	};

	std::vector<uint8_t> stream;
	std::string order;
	size_t at = 8;
	while (at + 12 <= file.size()) {
		const uint32_t length = read32(at);
		REQUIRE(at + 12 + length <= file.size());

		const std::string type(reinterpret_cast<const char *>(&file[at + 4]), 4);
		order += type + " ";
		CHECK(crc(&file[at + 4], length + 4) == read32(at + 8 + length));

		if (type == "IHDR") {
			CHECK(read32(at + 8) == image.Side);
			CHECK(read32(at + 12) == image.Side);
			CHECK(file[at + 16] == 8); // eight bits a channel
			CHECK(file[at + 17] == 6); // truecolour with alpha
		}
		if (type == "IDAT") {
			stream.insert(
				stream.end(),
				file.begin() + static_cast<long>(at + 8),
				file.begin() + static_cast<long>(at + 8 + length)
			);
		}
		at += 12 + length;
	}
	CHECK(at == file.size());
	CHECK(order == "IHDR IDAT IEND ");

	// The zlib stream: a header, stored blocks, and an Adler-32 that has to
	// match the bytes it claims to carry.
	REQUIRE(stream.size() > 6);
	CHECK(stream[0] == 0x78);

	std::vector<uint8_t> raw;
	size_t walk = 2;
	bool last = false;
	while (!last && walk + 5 <= stream.size()) {
		last = (stream[walk] & 1u) != 0u;
		const size_t count =
			static_cast<size_t>(stream[walk + 1]) | (static_cast<size_t>(stream[walk + 2]) << 8);
		REQUIRE(walk + 5 + count <= stream.size());
		raw.insert(
			raw.end(),
			stream.begin() + static_cast<long>(walk + 5),
			stream.begin() + static_cast<long>(walk + 5 + count)
		);
		walk += 5 + count;
	}
	CHECK(last);

	// One filter byte plus one row of pixels, per row.
	CHECK(raw.size() == (static_cast<size_t>(image.Side) * 4 + 1) * image.Side);

	uint32_t low = 1;
	uint32_t high = 0;
	for (const uint8_t byte : raw) {
		low = (low + byte) % 65521u;
		high = (high + low) % 65521u;
	}
	const size_t tail = stream.size() - 4;
	CHECK(
		((high << 16) | low) ==
		((static_cast<uint32_t>(stream[tail]) << 24) | (static_cast<uint32_t>(stream[tail + 1]) << 16) |
		 (static_cast<uint32_t>(stream[tail + 2]) << 8) | static_cast<uint32_t>(stream[tail + 3]))
	);

	// And the pixels are the ones that went in, in the order a reader expects.
	for (uint32_t y = 0; y < image.Side; y++) {
		const size_t row = y * (static_cast<size_t>(image.Side) * 4 + 1);
		CHECK(raw[row] == 0);
		CHECK(raw[row + 1] == image.Rgba[static_cast<size_t>(y) * image.Side * 4]);
	}
}

TEST_CASE("an inspector is picked from what a node produced", "[studio][nodegraph]") {
	Types();
	RegisterInspectors();

	Graph graph;
	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	const NodeId average = graph.Add("field.readout", 300.0f, 0.0f);
	const NodeId sum = graph.Add("number.arithmetic", 600.0f, 0.0f);
	const NodeId note = graph.Add("graph.note", 900.0f, 0.0f);
	const NodeId staged = graph.Add("task.staged", 0.0f, 400.0f);
	graph.Find(noise)->Widgets["resolution"].Text = "64";
	REQUIRE(graph.Connect(noise, "Out", average, "In") == LinkResult::Made);
	REQUIRE(graph.Connect(average, "Value", sum, "A") == LinkResult::Made);

	const auto pick = [&graph](const Evaluator &runner, NodeId id) {
		Inspection what;
		what.Node = graph.Find(id);
		what.Type = NodeTypes::Find(what.Node->Type);
		what.Graph = &graph;
		what.Runner = &runner;
		return Inspectors::For(what);
	};

	Evaluator runner;

	// **Before anything has run, everything is empty** — which is the honest
	// answer and not an empty picture frame that reads as a broken preview.
	CHECK(pick(runner, noise) == Inspectors::Find("empty"));

	runner.Run(graph);

	// A node whose payload draws gets the picture panel; one where nothing at
	// either end draws gets the readout; a staged one is about its run whether
	// or not it finished; and a node with no evaluation was never going to say
	// anything.
	CHECK(pick(runner, noise) == Inspectors::Find("field"));
	CHECK(pick(runner, sum) == Inspectors::Find("value"));
	CHECK(pick(runner, staged) == Inspectors::Find("run"));
	CHECK(pick(runner, note) == Inspectors::Find("empty"));

	// **An input counts, and that is deliberate.** A readout produces one
	// number, but the thing worth looking at when it is selected is the field it
	// was handed — so it gets the picture panel, whose input strip is what shows
	// it.
	CHECK(pick(runner, average) == Inspectors::Find("field"));

	// The type's own choice wins over the inference.
	NodeType said = *NodeTypes::Find("field.perlin");
	said.Id = "field.perlin.said";
	said.Inspector = "value";
	NodeTypes::Register(said);

	const NodeId told = graph.Add("field.perlin.said", 0.0f, 800.0f);
	CHECK(pick(runner, told) == Inspectors::Find("value"));
}

TEST_CASE("a bad document is refused and a bad line is not fatal", "[studio][nodegraph]") {
	Types();
	Graph graph;
	std::string error;

	CHECK_FALSE(Load("<html>", graph, error));
	CHECK_FALSE(error.empty());

	error.clear();
	REQUIRE(Load(
		"nodegraph 1\n"
		"node | 1 | field.perlin | 0 | 0 |\n"
		"node | 2 | nobody.knows.this | 100 | 0 |\n"
		"link | 1 | Out | 2 | In\n"
		"garbage\n",
		graph,
		error
	));

	// The unregistered node is kept rather than dropped — a graph saved with
	// something's node type has to survive being opened without it — and the
	// link that cannot be made is dropped rather than refusing the file.
	CHECK(graph.Nodes().size() == 2);
	CHECK(graph.Links().empty());
}

// --- the canvas ---------------------------------------------------------------

TEST_CASE("the canvas draws a graph without tripping imgui", "[studio][nodegraph]") {
	Types();
	const Context context;

	Graph graph;
	BuildDemoGraph(graph);
	CHECK(graph.Nodes().size() == 11);
	CHECK(graph.Links().size() == 11);

	Canvas canvas;
	Evaluator runner;
	canvas.Observe(&runner);
	runner.Run(graph);

	// Three frames, because the first submits a window imgui has never seen and
	// the second is the one with a real layout in it.
	for (int frame = 0; frame < 3; frame++) {
		Frame(canvas, graph, 500.0f, 400.0f, false);
	}

	// Nothing was drawn into the model by drawing it.
	CHECK(graph.Nodes().size() == 11);
	CHECK(graph.Links().size() == 11);
	CHECK(canvas.Selection().empty());

	// **And again with a frame and a fold in it**, which are the two things that
	// change what the canvas walks: a group is drawn behind everything from
	// bounds computed on the spot, and a fold hides its members and re-routes
	// every wire crossing it. Both are new draw paths that push and pop, and an
	// imbalance there is an assertion frames later in an unrelated panel.
	std::vector<NodeId> some;
	for (const Node &node : graph.Nodes()) {
		if (some.size() < 3) {
			some.push_back(node.Id);
		}
	}
	REQUIRE(graph.Group({some[0], some[1]}, "Sources", Colour::Hex(0x4ADE80)) != NO_GROUP);

	const NodeId folded = graph.Compress({some[1], some[2]}, 400.0f, 200.0f);
	REQUIRE(folded != NO_NODE);
	runner.Run(graph);

	for (int frame = 0; frame < 3; frame++) {
		Frame(canvas, graph, 500.0f, 400.0f, false);
	}

	// Inside the fold, only its members are drawn — and drawing at depth is
	// still just drawing.
	canvas.Enter(graph, folded);
	CHECK(canvas.Inside() == folded);
	CHECK(canvas.Path().size() == 1);
	for (int frame = 0; frame < 3; frame++) {
		Frame(canvas, graph, 500.0f, 400.0f, false);
	}

	canvas.Leave(graph);
	CHECK(canvas.Inside() == NO_NODE);
	CHECK(graph.Nodes().size() == 12);
	CHECK(graph.Links().size() == 11);
}

TEST_CASE("a drag from an output to an input makes the link", "[studio][nodegraph]") {
	Types();
	const Context context;

	Graph graph;
	const NodeId noise = graph.Add("field.perlin", 20.0f, 20.0f);
	const NodeId combine = graph.Add("field.combine", 400.0f, 20.0f);

	Canvas canvas;
	Frame(canvas, graph, 0.0f, 0.0f, false);

	float fromX = 0.0f;
	float fromY = 0.0f;
	float toX = 0.0f;
	float toY = 0.0f;
	ScreenPort(*graph.Find(noise), "Out", false, fromX, fromY);
	ScreenPort(*graph.Find(combine), "A", true, toX, toY);

	// Press on the output, move to the input, release. Three frames, because a
	// press and a release in one is a click imgui never reports as a drag.
	Frame(canvas, graph, fromX, fromY, true);
	Frame(canvas, graph, toX, toY, true);
	Frame(canvas, graph, toX, toY, false);

	REQUIRE(graph.Links().size() == 1);
	CHECK(graph.Links().front().From == noise);
	CHECK(graph.Links().front().To == combine);
	CHECK(graph.Links().front().ToPort == "A");
	CHECK(canvas.LastRefusal.empty());
}

TEST_CASE("a drag onto a port of the wrong type is refused, and says so", "[studio][nodegraph]") {
	Types();
	const Context context;

	Graph graph;
	const NodeId number = graph.Add("number.constant", 20.0f, 20.0f);
	const NodeId combine = graph.Add("field.combine", 400.0f, 20.0f);

	Canvas canvas;
	Frame(canvas, graph, 0.0f, 0.0f, false);

	float fromX = 0.0f;
	float fromY = 0.0f;
	float toX = 0.0f;
	float toY = 0.0f;
	ScreenPort(*graph.Find(number), "Out", false, fromX, fromY);
	ScreenPort(*graph.Find(combine), "A", true, toX, toY);

	Frame(canvas, graph, fromX, fromY, true);
	Frame(canvas, graph, toX, toY, true);
	Frame(canvas, graph, toX, toY, false);

	// **No link, and a sentence.** A refusal that left no trace would look
	// exactly like a drag that missed the port.
	CHECK(graph.Links().empty());
	CHECK(canvas.LastRefusal == std::string(Describe(LinkResult::TypeMismatch)));
}

TEST_CASE("clicking a node selects it and dragging moves it", "[studio][nodegraph]") {
	Types();
	const Context context;

	Graph graph;
	const NodeId noise = graph.Add("field.perlin", 60.0f, 60.0f);

	Canvas canvas;
	Frame(canvas, graph, 0.0f, 0.0f, false);

	const ImVec2 padding = ImGui::GetStyle().WindowPadding;
	const float bodyX = WINDOW_X + padding.x + 60.0f + 90.0f;
	const float bodyY = WINDOW_Y + padding.y + 60.0f + 8.0f;

	Frame(canvas, graph, bodyX, bodyY, true);
	REQUIRE(canvas.Selection().size() == 1);
	CHECK(canvas.Selection().front() == noise);

	Frame(canvas, graph, bodyX + 50.0f, bodyY + 30.0f, true);
	Frame(canvas, graph, bodyX + 50.0f, bodyY + 30.0f, false);

	// The node moved with the pointer, and moving it changed no result — which
	// is what `Hash` leaving position out is for.
	CHECK(graph.Find(noise)->X > 60.0f);
	CHECK(graph.Find(noise)->Y > 60.0f);
}

// --- the async half -----------------------------------------------------------

namespace {
	// Drives an evaluator until nothing is working, with a ceiling so a case
	// fails rather than hangs when something never finishes.
	//
	// **A poll rather than a wait**, because that is what the panel does: `Run`
	// is called once a frame, collects whatever landed and returns.
	RunReport Settle(Evaluator &runner, const Graph &graph, int milliseconds = 8000) {
		RunReport report = runner.Run(graph);
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);

		while ((runner.Busy() || report.Waiting > 0) && std::chrono::steady_clock::now() < until) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			report = runner.Run(graph);
		}
		return report;
	}
}

TEST_CASE("an async node runs off the caller's thread and reports as it goes", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId task = graph.Add("task.staged", 0.0f, 0.0f);
	REQUIRE(task != NO_NODE);
	graph.Find(task)->Widgets["seconds"].Number = 0.4;

	Evaluator runner;

	// **The first `Run` returns while the work is still going.** That is the
	// whole difference from a sync node: if this blocked, the editor would stop
	// drawing for as long as the node takes.
	const auto began = std::chrono::steady_clock::now();
	const RunReport first = runner.Run(graph);
	const auto returned = std::chrono::steady_clock::now();

	CHECK(first.Started == 1);
	CHECK(first.Running == 1);
	CHECK(first.Evaluated == 0);
	CHECK(runner.Busy());
	CHECK(std::chrono::duration<double>(returned - began).count() < 0.2);

	// Nothing is published until it finishes — a half-computed result is worse
	// than none, because the cache would hold it under a hash that says it is
	// the answer.
	CHECK(runner.Output(task, "Done") == nullptr);
	CHECK(runner.Status(task).State == NodeState::Running);

	const RunReport last = Settle(runner, graph);
	CHECK(last.Running == 0);
	CHECK_FALSE(runner.Busy());

	REQUIRE(runner.Output(task, "Done") != nullptr);
	CHECK(std::any_cast<double>(*runner.Output(task, "Done")) == 1.0);

	const NodeStatus status = runner.Status(task);
	CHECK(status.State == NodeState::Done);
	CHECK(status.Progress == 1.0f);
	CHECK(status.Milliseconds > 0.0);

	// And it caches like everything else: the second run recomputes nothing.
	const RunReport again = runner.Run(graph);
	CHECK(again.Cached == 1);
	CHECK(again.Started == 0);
}

TEST_CASE("two branches that do not feed each other run at once", "[studio][nodegraph]") {
	Types();
	Graph graph;

	// **Half a second each, and the assertion has a wide margin.** What is being
	// checked is that these overlap at all — a scheduler that ran them one after
	// the other would take twice as long, and the bound is far enough from both
	// numbers that a slow machine does not decide the outcome.
	const NodeId left = graph.Add("task.staged", 0.0f, 0.0f);
	const NodeId right = graph.Add("task.staged", 0.0f, 200.0f);
	graph.Find(left)->Widgets["seconds"].Number = 0.5;
	graph.Find(right)->Widgets["seconds"].Number = 0.5;

	// **Different labels, so they are two hashes.** Identical nodes are one
	// piece of work by design — the evaluator makes the second wait on the first
	// — which is the right behaviour and would make this case measure nothing.
	graph.Find(left)->Widgets["label"].Text = "left";
	graph.Find(right)->Widgets["label"].Text = "right";

	Evaluator runner;

	const auto began = std::chrono::steady_clock::now();
	const RunReport report = Settle(runner, graph);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

	CHECK(report.Running == 0);
	CHECK(runner.Output(left, "Done") != nullptr);
	CHECK(runner.Output(right, "Done") != nullptr);

	INFO("took " << seconds << "s");
	CHECK(seconds < 0.85);
}

TEST_CASE("a node waits for an input that is still being computed", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId task = graph.Add("task.staged", 0.0f, 0.0f);
	const NodeId doubled = graph.Add("number.arithmetic", 240.0f, 0.0f);
	graph.Find(task)->Widgets["seconds"].Number = 0.4;
	graph.Find(doubled)->Widgets["op"].Text = "add";
	REQUIRE(graph.Connect(task, "Done", doubled, "A") == LinkResult::Made);

	Evaluator runner;
	const RunReport first = runner.Run(graph);

	// **Waiting, not evaluated.** Running it now would read the unconnected
	// fallback — zero — and cache that under a hash which says the input was the
	// task's result. The wrong answer would then be permanent.
	CHECK(first.Waiting == 1);
	CHECK(first.Evaluated == 0);
	CHECK(runner.Output(doubled, "Out") == nullptr);

	const RunReport last = Settle(runner, graph);
	CHECK(last.Waiting == 0);
	REQUIRE(runner.Output(doubled, "Out") != nullptr);

	// One from the task plus nothing on B.
	CHECK(std::any_cast<double>(*runner.Output(doubled, "Out")) == 1.0);
}

TEST_CASE("an evaluator with work in flight can still be destroyed", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId task = graph.Add("task.staged", 0.0f, 0.0f);
	graph.Find(task)->Widgets["seconds"].Number = 6.0;

	const auto began = std::chrono::steady_clock::now();
	{
		Evaluator runner;
		runner.Run(graph);
		REQUIRE(runner.Busy());
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

	// **The editor closes while something long is running, often.** The task
	// polls `Inputs::Cancelled` between slices, so shutdown costs a slice rather
	// than the six seconds it was asked for.
	INFO("shutdown took " << seconds << "s");
	CHECK(seconds < 1.5);
}

TEST_CASE("erosion changes the field it is given, and caches the result", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	const NodeId eroded = graph.Add("field.erode", 260.0f, 0.0f);
	graph.Find(noise)->Widgets["resolution"].Text = "64";
	graph.Find(eroded)->Widgets["thermal"].Number = 6.0;
	graph.Find(eroded)->Widgets["hydraulic"].Number = 3.0;
	REQUIRE(graph.Connect(noise, "Out", eroded, "In") == LinkResult::Made);

	Evaluator runner;
	const RunReport report = Settle(runner, graph);

	CHECK(report.Running == 0);
	REQUIRE(runner.Output(noise, "Out") != nullptr);
	REQUIRE(runner.Output(eroded, "Out") != nullptr);

	// The two payloads are the same shape and not the same values — an erosion
	// that returned its input would pass every check that only looked at types.
	const std::any &before = *runner.Output(noise, "Out");
	const std::any &after = *runner.Output(eroded, "Out");

	// **Drawn through `PictureOf`, which is what the canvas and the inspector
	// both call.** The picture comes from the *wire* rather than from the node
	// type — that is what lets a panel draw a node's inputs, whose payloads were
	// made upstream by a type it never heard of.
	PreviewImage first;
	PreviewImage second;
	const NodeType *type = NodeTypes::Find("field.erode");
	REQUIRE(type != nullptr);
	REQUIRE(HasPicture(*type));
	REQUIRE(PictureOf(type, "data.FIELD", before, first));
	REQUIRE(PictureOf(type, "data.FIELD", after, second));

	CHECK(first.Valid());
	CHECK(second.Valid());
	CHECK(first.Side == second.Side);
	CHECK(first.Rgba != second.Rgba);

	// Deterministic: the same hash is the same picture, which is what lets the
	// canvas key a texture on it.
	PreviewImage third;
	REQUIRE(PictureOf(type, "data.FIELD", after, third));
	CHECK(third.Rgba == second.Rgba);

	// And a node that declared no preview of its own still has one, because the
	// wire it produces on does.
	CHECK_FALSE(static_cast<bool>(type->Preview));
}

TEST_CASE("a colourised field is a picture and a height field is a grey one", "[studio][nodegraph]") {
	Types();
	Graph graph;

	const NodeId noise = graph.Add("field.perlin", 0.0f, 0.0f);
	const NodeId picture = graph.Add("image.colourise", 260.0f, 0.0f);
	graph.Find(noise)->Widgets["resolution"].Text = "64";
	REQUIRE(graph.Connect(noise, "Out", picture, "In") == LinkResult::Made);

	Evaluator runner;
	Settle(runner, graph);

	const NodeType *field = NodeTypes::Find("field.perlin");
	const NodeType *colour = NodeTypes::Find("image.colourise");
	REQUIRE(field != nullptr);
	REQUIRE(colour != nullptr);

	PreviewImage grey;
	REQUIRE(PictureOf(field, "data.FIELD", *runner.Output(noise, "Out"), grey));
	REQUIRE(grey.Valid());

	// A height field previews as light: every pixel has one value in three
	// channels, which is the whole reason `image.colourise` is a node rather
	// than a setting on the viewer.
	bool coloured = false;
	for (size_t at = 0; at + 3 < grey.Rgba.size(); at += 4) {
		coloured = coloured || grey.Rgba[at] != grey.Rgba[at + 1] || grey.Rgba[at + 1] != grey.Rgba[at + 2];
	}
	CHECK_FALSE(coloured);

	PreviewImage painted;
	REQUIRE(PictureOf(colour, "data.IMAGE", *runner.Output(picture, "Out"), painted));
	REQUIRE(painted.Valid());

	for (size_t at = 0; at + 3 < painted.Rgba.size(); at += 4) {
		coloured = coloured || painted.Rgba[at] != painted.Rgba[at + 1] ||
				   painted.Rgba[at + 1] != painted.Rgba[at + 2];
	}
	CHECK(coloured);

	// A preview of something that is not what the node makes answers no rather
	// than reinterpreting the bytes.
	PreviewImage refused;
	CHECK_FALSE(PictureOf(colour, "data.IMAGE", std::any(42.0), refused));
	CHECK_FALSE(PictureOf(field, "data.FIELD", std::any(std::string("not a field")), refused));

	// And a wire nobody taught to draw itself says so rather than guessing —
	// which is what keeps a number out of a grey square.
	CHECK_FALSE(PictureOf(nullptr, "data.NUMBER", std::any(0.5), refused));
	CHECK(DescribeValue("data.NUMBER", std::any(0.5)) == "0.5000");
}

TEST_CASE("the demo graph is wired, and settles", "[studio][nodegraph]") {
	Types();
	Graph graph;
	BuildDemoGraph(graph);

	CHECK(graph.Nodes().size() == 11);
	CHECK(graph.Links().size() == 11);

	// Small, so the case is about the wiring rather than about erosion's cost.
	for (Node &node : graph.Nodes()) {
		if (const auto found = node.Widgets.find("resolution"); found != node.Widgets.end()) {
			found->second.Text = "64";
		}
		if (node.Type == "task.staged") {
			node.Widgets["seconds"].Number = 0.2;
		}
		if (node.Type == "field.erode") {
			node.Widgets["thermal"].Number = 4.0;
			node.Widgets["hydraulic"].Number = 2.0;
		}
	}

	Evaluator runner;
	const RunReport report = Settle(runner, graph);

	CHECK(report.Running == 0);
	CHECK(report.Waiting == 0);

	// Every node that computes something has produced it — a chain that stopped
	// half way would leave a node with no output and nothing saying why.
	for (const Node &node : graph.Nodes()) {
		const NodeType *type = NodeTypes::Find(node.Type);
		REQUIRE(type != nullptr);
		if (!type->Evaluate || type->Outputs.empty()) {
			continue;
		}
		INFO(node.Type);
		CHECK(runner.Output(node.Id, type->Outputs.front().Name) != nullptr);
	}
}
