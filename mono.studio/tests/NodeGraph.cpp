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
#include <thread>
#include <string>
#include <studio/NodeGraph.hpp>
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
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
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

	PreviewImage first;
	PreviewImage second;
	const NodeType *type = NodeTypes::Find("field.erode");
	REQUIRE(type != nullptr);
	REQUIRE(type->Preview);
	REQUIRE(type->Preview(before, first));
	REQUIRE(type->Preview(after, second));

	CHECK(first.Valid());
	CHECK(second.Valid());
	CHECK(first.Side == second.Side);
	CHECK(first.Rgba != second.Rgba);

	// Deterministic: the same hash is the same picture, which is what lets the
	// canvas key a texture on it.
	PreviewImage third;
	REQUIRE(type->Preview(after, third));
	CHECK(third.Rgba == second.Rgba);
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
	REQUIRE(field->Preview(*runner.Output(noise, "Out"), grey));
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
	REQUIRE(colour->Preview(*runner.Output(picture, "Out"), painted));
	REQUIRE(painted.Valid());

	for (size_t at = 0; at + 3 < painted.Rgba.size(); at += 4) {
		coloured = coloured || painted.Rgba[at] != painted.Rgba[at + 1] ||
				   painted.Rgba[at + 1] != painted.Rgba[at + 2];
	}
	CHECK(coloured);

	// A preview of something that is not what the node makes answers no rather
	// than reinterpreting the bytes.
	PreviewImage refused;
	CHECK_FALSE(colour->Preview(std::any(42.0), refused));
	CHECK_FALSE(field->Preview(std::any(std::string("not a field")), refused));
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
