#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.angview.view")
TEST_DEPENDS("engine.angview.layout")
TEST_DEPENDS("engine.ang.compile")
TEST_TIER("fast")

using namespace engine::angview;
using engine::ang::Graph;
using engine::ang::Link;
using engine::core::Vector2;
namespace edit = engine::ang::edit;

namespace {
	struct Scene {
		engine::ang::Registry Types = tests::MakeRegistry();
		Graph Document;
		std::unique_ptr<View> Canvas;

		Scene() {
			tests::Add(Document, "one", "math.number", {0.0f, 0.0f});
			tests::Add(Document, "add", "math.add", {300.0f, 0.0f});
			tests::Add(Document, "text", "text.upper", {0.0f, 300.0f});
			Canvas = std::make_unique<View>(Document, Types);
			Canvas->SetViewport({1000.0f, 800.0f});
		}

		void Undo() {
			// Newest first, as an undo stack would.
			auto records = Canvas->TakeEdits();
			for (auto record = records.rbegin(); record != records.rend(); ++record) {
				REQUIRE(Document.Apply(std::span<const engine::ang::Edit>(record->Inverse)).Ok);
			}
		}
	};
}

TEST_CASE("dragging a node previews, then moves it with one undoable edit", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	view.PointerDown(PointerButton::Left, {50.0f, 10.0f});
	CHECK(view.Dragging() == DragKind::Nodes);
	CHECK(view.Selection() == std::vector<std::string>{"one"});
	view.PointerMove({90.0f, 30.0f});
	// The document waits for release; the layout shows the preview.
	CHECK(scene.Document.FindNode("one")->Position.X == 0.0f);
	CHECK(view.Geometry().Find("one")->Bounds.Min.X == 40.0f);
	view.PointerUp(PointerButton::Left, {90.0f, 30.0f});
	CHECK(scene.Document.FindNode("one")->Position.X == 40.0f);
	CHECK(scene.Document.FindNode("one")->Position.Y == 20.0f);
	const auto records = view.TakeEdits();
	REQUIRE(records.size() == 1);
	REQUIRE(scene.Document.Apply(std::span<const engine::ang::Edit>(records[0].Inverse)).Ok);
	CHECK(scene.Document.FindNode("one")->Position.X == 0.0f);
}

TEST_CASE("a selection moves together and zoom scales the drag", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	view.Select({"one", "add"});
	view.SetPanZoom({0.0f, 0.0f}, 2.0f);
	tests::Drag(view, {100.0f, 20.0f}, {140.0f, 20.0f});
	CHECK(scene.Document.FindNode("one")->Position.X == 20.0f);
	CHECK(scene.Document.FindNode("add")->Position.X == 320.0f);
	CHECK(scene.Document.FindNode("text")->Position.X == 0.0f);
}

TEST_CASE("middle, Alt and Space drags pan, and Escape puts a pan back", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	tests::Drag(view, {500.0f, 500.0f}, {520.0f, 530.0f}, PointerButton::Middle);
	CHECK(view.Pan().X == 20.0f);
	CHECK(view.Pan().Y == 30.0f);
	tests::Drag(view, {500.0f, 500.0f}, {510.0f, 500.0f}, PointerButton::Left, {.Alt = true});
	CHECK(view.Pan().X == 30.0f);
	view.KeyDown(Key::Space);
	tests::Drag(view, {50.0f, 10.0f}, {60.0f, 10.0f});
	view.KeyUp(Key::Space);
	CHECK(view.Pan().X == 40.0f);
	// Space-drag over a node pans rather than moving it.
	CHECK(scene.Document.FindNode("one")->Position.X == 0.0f);
	view.PointerDown(PointerButton::Middle, {0.0f, 0.0f});
	view.PointerMove({100.0f, 100.0f});
	view.KeyDown(Key::Escape);
	CHECK(view.Pan().X == 40.0f);
	CHECK(view.Dragging() == DragKind::None);
}

TEST_CASE("the wheel zooms about the cursor", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	const Vector2 cursor{400.0f, 300.0f};
	const Vector2 before = view.ToGraph(cursor);
	view.Wheel(3.0f, cursor);
	CHECK(view.Zoom() > 1.3f);
	const Vector2 after = view.ToGraph(cursor);
	CHECK(std::abs(after.X - before.X) < 1e-3f);
	CHECK(std::abs(after.Y - before.Y) < 1e-3f);
	for (int step = 0; step < 100; ++step) {
		view.Wheel(-1.0f, cursor);
	}
	CHECK(view.Zoom() == 0.1f);
	CHECK(!view.Wheel(0.0f, cursor));
}

TEST_CASE("pulling a wire onto a compatible port links it", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	const Vector2 from = tests::OutputPort({0.0f, 0.0f}, 0);
	const Vector2 to = tests::InputPort({300.0f, 0.0f}, 1);
	view.PointerDown(PointerButton::Left, from);
	CHECK(view.Dragging() == DragKind::Wire);
	view.PointerMove(to);
	REQUIRE(view.Wire().has_value());
	CHECK(view.Wire()->TargetAccepts);
	view.PointerUp(PointerButton::Left, to);
	REQUIRE(scene.Document.Links().size() == 1);
	CHECK(scene.Document.Links()[0] == Link{"one", "value", "add", "b"});
	scene.Undo();
	CHECK(scene.Document.Links().empty());
}

TEST_CASE("an incompatible port refuses with a reason and links nothing", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	const Vector2 from = tests::OutputPort({0.0f, 0.0f}, 0);
	const Vector2 to = tests::InputPort({0.0f, 300.0f}, 0);
	view.PointerDown(PointerButton::Left, from);
	view.PointerMove(to);
	REQUIRE(view.Wire().has_value());
	CHECK(!view.Wire()->TargetAccepts);
	view.PointerUp(PointerButton::Left, to);
	CHECK(scene.Document.Links().empty());
	CHECK(view.LastError().find("does not connect") != std::string::npos);
}

TEST_CASE("a wire can start at an input and end at an output", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	tests::Drag(view, tests::InputPort({300.0f, 0.0f}, 0), tests::OutputPort({0.0f, 0.0f}, 0));
	REQUIRE(scene.Document.Links().size() == 1);
	CHECK(scene.Document.Links()[0] == Link{"one", "value", "add", "a"});
}

TEST_CASE("dropping on a full one-wire input replaces its wire in one edit", "[angview][view]") {
	Scene scene;
	tests::Add(scene.Document, "two", "math.number", {0.0f, 150.0f});
	REQUIRE(scene.Document.Apply(edit::AddLink{{"one", "value", "add", "a"}}).Ok);
	View &view = *scene.Canvas;
	view.GraphChanged();
	tests::Drag(view, tests::OutputPort({0.0f, 150.0f}, 0), tests::InputPort({300.0f, 0.0f}, 0));
	REQUIRE(scene.Document.Links().size() == 1);
	CHECK(scene.Document.Links()[0] == Link{"two", "value", "add", "a"});
	CHECK(view.TakeEdits().size() == 1);
}

TEST_CASE("grabbing a linked input picks the wire up", "[angview][view]") {
	Scene scene;
	REQUIRE(scene.Document.Apply(edit::AddLink{{"one", "value", "add", "a"}}).Ok);
	View &view = *scene.Canvas;
	view.GraphChanged();
	// Re-seat on the other input: one move, one record.
	tests::Drag(view, tests::InputPort({300.0f, 0.0f}, 0), tests::InputPort({300.0f, 0.0f}, 1));
	REQUIRE(scene.Document.Links().size() == 1);
	CHECK(scene.Document.Links()[0] == Link{"one", "value", "add", "b"});
	CHECK(view.TakeEdits().size() == 1);
	// Drop on nothing: disconnect.
	tests::Drag(view, tests::InputPort({300.0f, 0.0f}, 1), {700.0f, 600.0f});
	CHECK(scene.Document.Links().empty());
	CHECK(!view.AddPalette().Open);
	// Ctrl keeps the old wire and starts a fresh one from the input.
	REQUIRE(scene.Document.Apply(edit::AddLink{{"one", "value", "add", "a"}}).Ok);
	view.PointerDown(PointerButton::Left, tests::InputPort({300.0f, 0.0f}, 0), {.Control = true});
	CHECK(view.Wire()->Dir == engine::ang::Direction::Input);
	view.KeyDown(Key::Escape);
	CHECK(scene.Document.Links().size() == 1);
}

TEST_CASE("dropping a wire on empty space opens a palette filtered by type", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	tests::Drag(view, tests::OutputPort({0.0f, 0.0f}, 0), {600.0f, 500.0f});
	const Palette &palette = view.AddPalette();
	REQUIRE(palette.Open);
	REQUIRE(palette.Wire.has_value());
	// Number output: only nodes taking a number input.
	CHECK(palette.Matches == std::vector<std::string>{"math.add", "math.total"});
	view.TextInput("tot");
	CHECK(view.AddPalette().Matches == std::vector<std::string>{"math.total"});
	view.KeyDown(Key::Enter);
	CHECK(!view.AddPalette().Open);
	const engine::ang::NodeInstance *made = scene.Document.FindNode("total1");
	REQUIRE(made != nullptr);
	CHECK(made->Position.X == 600.0f);
	REQUIRE(scene.Document.Links().size() == 1);
	CHECK(scene.Document.Links()[0] == Link{"one", "value", "total1", "values"});
	CHECK(view.Selection() == std::vector<std::string>{"total1"});
	// Node and link arrive as one undo step.
	CHECK(view.TakeEdits().size() == 1);
}

TEST_CASE("the palette opens on Tab and right click, and navigates by keys and clicks", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	view.PointerMove({700.0f, 600.0f});
	view.KeyDown(Key::Tab);
	REQUIRE(view.AddPalette().Open);
	CHECK(view.AddPalette().Matches.size() == 4);
	view.KeyDown(Key::Down);
	view.KeyDown(Key::Down);
	view.KeyDown(Key::Up);
	CHECK(view.AddPalette().Highlighted == 1);
	view.TextInput("zz");
	CHECK(view.AddPalette().Matches.empty());
	view.KeyDown(Key::Backspace);
	view.KeyDown(Key::Backspace);
	CHECK(view.AddPalette().Matches.size() == 4);
	view.KeyDown(Key::Escape);
	CHECK(!view.AddPalette().Open);
	view.PointerDown(PointerButton::Right, {700.0f, 600.0f});
	REQUIRE(view.AddPalette().Open);
	// Second row of the palette: the second match.
	const float row = 13.0f + 6.0f;
	view.PointerDown(PointerButton::Left, {710.0f, 600.0f + row * 2.0f + 2.0f});
	CHECK(!view.AddPalette().Open);
	CHECK(scene.Document.FindNode("add1") != nullptr);
	// Clicking away closes without adding.
	view.KeyDown(Key::Tab);
	view.PointerDown(PointerButton::Left, {5.0f, 790.0f});
	view.PointerUp(PointerButton::Left, {5.0f, 790.0f});
	CHECK(!view.AddPalette().Open);
	CHECK(scene.Document.Nodes().size() == 4);
}

TEST_CASE("box select replaces or adds, and a click on nothing clears", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	tests::Drag(view, {-20.0f, -20.0f}, {200.0f, 400.0f});
	CHECK(view.Selection() == std::vector<std::string>{"one", "text"});
	tests::Drag(view, {280.0f, -20.0f}, {500.0f, 50.0f}, PointerButton::Left, {.Shift = true});
	CHECK(view.Selection().size() == 3);
	view.PointerDown(PointerButton::Left, {900.0f, 700.0f});
	CHECK(view.BoxSelection() == std::nullopt);
	view.PointerUp(PointerButton::Left, {900.0f, 700.0f});
	CHECK(view.Selection().empty());
	// Shift-click toggles one node.
	view.PointerDown(PointerButton::Left, {50.0f, 10.0f}, {.Shift = true});
	view.PointerUp(PointerButton::Left, {50.0f, 10.0f}, {.Shift = true});
	view.PointerDown(PointerButton::Left, {350.0f, 10.0f}, {.Shift = true});
	view.PointerUp(PointerButton::Left, {350.0f, 10.0f}, {.Shift = true});
	CHECK(view.Selection() == std::vector<std::string>{"one", "add"});
	view.PointerDown(PointerButton::Left, {50.0f, 10.0f}, {.Shift = true});
	view.PointerUp(PointerButton::Left, {50.0f, 10.0f}, {.Shift = true});
	CHECK(view.Selection() == std::vector<std::string>{"add"});
}

TEST_CASE("Delete removes the selection, Ctrl+A selects all, Ctrl-click cuts a wire", "[angview][view]") {
	Scene scene;
	REQUIRE(scene.Document.Apply(edit::AddLink{{"one", "value", "add", "a"}}).Ok);
	View &view = *scene.Canvas;
	view.GraphChanged();
	// The wire runs from (160, 40) to (300, 40); its middle is near (230, 40).
	view.PointerMove({230.0f, 40.0f});
	CHECK(view.Hovered().Kind == HitKind::Link);
	view.PointerDown(PointerButton::Left, {230.0f, 40.0f}, {.Control = true});
	view.PointerUp(PointerButton::Left, {230.0f, 40.0f});
	CHECK(scene.Document.Links().empty());
	CHECK(view.KeyDown(Key::A, {.Control = true}));
	CHECK(view.Selection().size() == 3);
	CHECK(!view.KeyDown(Key::A));
	CHECK(view.KeyDown(Key::Delete));
	CHECK(scene.Document.Nodes().empty());
	CHECK(view.Selection().empty());
	scene.Undo();
	CHECK(scene.Document.Nodes().size() == 3);
}

TEST_CASE("F frames the selection or the whole graph", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	view.SetPanZoom({-5000.0f, -5000.0f}, 3.0f);
	view.KeyDown(Key::F);
	const Rect bounds = view.Geometry().Bounds();
	const Vector2 min = view.ToView(bounds.Min);
	const Vector2 max = view.ToView(bounds.Max);
	CHECK(min.X >= 0.0f);
	CHECK(min.Y >= 0.0f);
	CHECK(max.X <= 1000.0f);
	CHECK(max.Y <= 800.0f);
	view.Select({"add"});
	view.Frame();
	const Vector2 centre = view.ToView(view.Geometry().Find("add")->Bounds.Centre());
	CHECK(std::abs(centre.X - 500.0f) < 0.5f);
	CHECK(std::abs(centre.Y - 400.0f) < 0.5f);
}

TEST_CASE("one finger drags and two fingers pan and pinch", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	view.TouchDown(1, {50.0f, 10.0f});
	view.TouchMove(1, {80.0f, 10.0f});
	view.TouchUp(1, {80.0f, 10.0f});
	CHECK(scene.Document.FindNode("one")->Position.X == 30.0f);

	view.TouchDown(1, {600.0f, 400.0f});
	view.TouchDown(2, {700.0f, 400.0f});
	CHECK(view.Dragging() == DragKind::Pinch);
	view.TouchMove(2, {800.0f, 400.0f});
	CHECK(std::abs(view.Zoom() - 2.0f) < 1e-3f);
	const Vector2 pan = view.Pan();
	view.TouchMove(1, {650.0f, 450.0f});
	view.TouchMove(2, {850.0f, 450.0f});
	CHECK(view.Pan().Y > pan.Y);
	view.TouchUp(2, {850.0f, 450.0f});
	view.TouchUp(1, {650.0f, 450.0f});
	CHECK(view.Dragging() == DragKind::None);
	// The node did not move during the pinch.
	CHECK(scene.Document.FindNode("one")->Position.X == 30.0f);
	CHECK(!view.TouchMove(9, {}));
}

TEST_CASE("a second finger cancels a node drag in progress", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	view.TouchDown(1, {50.0f, 10.0f});
	view.TouchMove(1, {150.0f, 10.0f});
	view.TouchDown(2, {600.0f, 400.0f});
	view.TouchUp(1, {150.0f, 10.0f});
	view.TouchUp(2, {600.0f, 400.0f});
	CHECK(scene.Document.FindNode("one")->Position.X == 0.0f);
}

TEST_CASE("dragging a group header moves the group and its members", "[angview][view]") {
	Scene scene;
	REQUIRE(scene.Document
				.Apply(
					edit::AddGroup{
						{.Id = "g", .Label = "Inputs", .Position = {-20.0f, -60.0f}, .Size = {220.0f, 160.0f}}
					}
				)
				.Ok);
	REQUIRE(scene.Document.Apply(edit::SetNodeGroup{"one", "g"}).Ok);
	View &view = *scene.Canvas;
	view.GraphChanged();
	CHECK(view.HitTest({0.0f, -50.0f}).Kind == HitKind::Group);
	tests::Drag(view, {0.0f, -50.0f}, {100.0f, -50.0f});
	CHECK(scene.Document.FindGroup("g")->Position.X == 80.0f);
	CHECK(scene.Document.FindNode("one")->Position.X == 100.0f);
	CHECK(scene.Document.FindNode("add")->Position.X == 300.0f);
	CHECK(view.TakeEdits().size() == 1);
}

TEST_CASE("paint rebuilds only after a visible change", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	int painted = 0;
	view.CustomPaint = [&](DrawList &list, const View &) {
		++painted;
		list.push_back({.Kind = DrawKind::Text, .Text = "custom"});
	};
	const DrawList &first = view.Paint();
	CHECK(painted == 1);
	CHECK(tests::HasText(first, "custom"));
	CHECK(tests::HasText(first, "Add"));
	CHECK(first.front().Fill == Rgba{0.0f, 0.0f, 0.0f, 1.0f});
	CHECK(!view.NeedsRedraw());
	view.Paint();
	CHECK(painted == 1);
	// Hover over empty space changes nothing visible.
	view.PointerMove({900.0f, 700.0f});
	CHECK(!view.NeedsRedraw());
	view.PointerMove({50.0f, 10.0f});
	CHECK(view.NeedsRedraw());
	view.Paint();
	REQUIRE(scene.Document.Apply(edit::MoveNode{"one", {5.0f, 5.0f}}).Ok);
	CHECK(view.NeedsRedraw());
	view.SetTheme(Theme::Blueprint());
	const DrawList &blue = view.Paint();
	CHECK(blue.front().Fill == Theme::Blueprint().Background);
	CHECK(painted == 3);
}

TEST_CASE("each wire style draws its own shapes", "[angview][view]") {
	Scene scene;
	REQUIRE(scene.Document.Apply(edit::AddLink{{"one", "value", "add", "a"}}).Ok);
	View &view = *scene.Canvas;
	const auto count = [&](DrawKind kind) {
		size_t total = 0;
		for (const DrawCommand &command : view.Paint()) {
			total += command.Kind == kind ? 1 : 0;
		}
		return total;
	};
	Theme theme = Theme::Minimal();
	theme.Wires = WireStyle::Bezier;
	view.SetTheme(theme);
	CHECK(count(DrawKind::Bezier) == 1);
	CHECK(count(DrawKind::Line) == 0);
	theme.Wires = WireStyle::Straight;
	view.SetTheme(theme);
	CHECK(count(DrawKind::Line) == 1);
	theme.Wires = WireStyle::Orthogonal;
	view.SetTheme(theme);
	CHECK(count(DrawKind::Line) == 3);
}

TEST_CASE("run reports colour nodes and subgraph members sum into their node", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	engine::ang::Plan plan;
	plan.Nodes.resize(3);
	plan.Nodes[0].Id = "one";
	plan.Nodes[1].Id = "add/inner";
	plan.Nodes[2].Id = "add/other";
	engine::ang::RunReport report;
	report.Nodes = {
		{.Status = engine::ang::NodeStatus::Ran, .Milliseconds = 1.5},
		{.Status = engine::ang::NodeStatus::Ran, .Milliseconds = 2.0, .Bytes = 10},
		{.Status = engine::ang::NodeStatus::Failed, .Note = "broke", .Milliseconds = 0.5, .Bytes = 5},
	};
	view.SetRunReport(plan, report);
	const NodeStatusView *add = view.StatusOf("add");
	REQUIRE(add != nullptr);
	CHECK(add->Status == engine::ang::NodeStatus::Failed);
	CHECK(add->Milliseconds == 2.5);
	CHECK(add->Bytes == 15);
	CHECK(add->Note == "broke");
	const DrawList &list = view.Paint();
	CHECK(tests::HasText(list, "1.50 ms"));
	CHECK(tests::HasText(list, "failed"));
	engine::ang::RunTimeline timeline;
	timeline.Push(
		{.RunNumber = 1,
		 .Milliseconds = 3.0,
		 .Samples = {{.Node = 0, .BeginNanoseconds = 0, .EndNanoseconds = 1500000}}}
	);
	view.SetTimeline(timeline);
	CHECK(tests::HasText(view.Paint(), "3.00 ms"));
	view.ClearRunFeedback();
	CHECK(view.StatusOf("add") == nullptr);
	CHECK(!tests::HasText(view.Paint(), "ms"));
}

TEST_CASE("a host can supply geometry and make empty drags pan", "[angview][view]") {
	Scene scene;
	View &view = *scene.Canvas;
	view.EmptyDragPans = true;
	// Host shapes: "one" is a 40x40 square at the origin with its output on the right edge.
	view.GeometrySource = [](Layout &layout) {
		layout.Add(
			{.Id = "one",
			 .Bounds = {{0.0f, 0.0f}, {40.0f, 40.0f}},
			 .Outputs = {{"value", "number", engine::ang::Direction::Output, {40.0f, 20.0f}}}}
		);
		layout.Add(
			{.Id = "add",
			 .Bounds = {{100.0f, 0.0f}, {140.0f, 40.0f}},
			 .Inputs = {{"a", "number", engine::ang::Direction::Input, {100.0f, 20.0f}}}}
		);
	};
	tests::Drag(view, {40.0f, 20.0f}, {100.0f, 20.0f});
	REQUIRE(scene.Document.Links().size() == 1);
	tests::Drag(view, {20.0f, 10.0f}, {30.0f, 10.0f});
	CHECK(scene.Document.FindNode("one")->Position.X == 10.0f);
	tests::Drag(view, {500.0f, 500.0f}, {520.0f, 500.0f});
	CHECK(view.Pan().X == 20.0f);
	CHECK(view.Selection() == std::vector<std::string>{"one"});
}

TEST_CASE("Ctrl+D duplicates the selection and the links inside it", "[angview][view]") {
	Scene scene;
	REQUIRE(scene.Document.Apply(edit::AddLink{{"one", "value", "add", "a"}}).Ok);
	View &view = *scene.Canvas;
	view.GraphChanged();
	CHECK(!view.KeyDown(Key::D, {.Control = true}));
	view.Select({"one", "add"});
	CHECK(!view.KeyDown(Key::D));
	REQUIRE(view.KeyDown(Key::D, {.Control = true}));
	CHECK(scene.Document.Nodes().size() == 5);
	CHECK(scene.Document.Links().size() == 2);
	CHECK(view.Selection().size() == 2);
	const auto *copy = scene.Document.FindNode(view.Selection().front());
	REQUIRE(copy != nullptr);
	CHECK(copy->Position.Y == 24.0f);
	CHECK(view.TakeEdits().size() == 1);
}
