#include "../src/Vector2Panel.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <studio/Vector2Editor.hpp>

TEST_SUITE_ID("studio.vector2editor")
TEST_DEPENDS("engine.imagegraph.vector2_presentation")
TEST_DEPENDS("studio.imagegraph")

using namespace engine::imagegraph;
using namespace studio;

namespace {
	std::string Input(int index) {
		const auto *entry = FindCatalogueEntry("pc.vector2");
		REQUIRE(entry);
		for (const auto &input : entry->Inputs)
			if (input.SourceIndex == index) return std::string(input.Id);
		FAIL("source Vector2 input is absent");
		return {};
	}
	Document Graph() {
		Document doc;
		doc.FormatVersion = 8;
		doc.Nodes = {{"point", "pc.vector2", "", {}, {{"x", 0.0}, {"y", 0.0}, {Input(3), int64_t{1}}}}};
		doc.Outputs = {{"point-value", "point", "vector"}};
		return doc;
	}
}

TEST_CASE("source coordinate pad maps upward Y and preserves its view center", "[studio][vector2_editor]") {
	Vector2PadView view;
	Vector2 value{99, 99};
	REQUIRE(Vector2PadCoordinate(view, {10, 20, 160, 160}, {50, 60}, false, value));
	CHECK(value.X == -.5);
	CHECK(value.Y == .5);
	REQUIRE(Vector2PadCoordinate(view, {10, 20, 160, 160}, {50, 60}, true, value));
	CHECK(value.X == 0);
	CHECK(value.Y == 0);
	CHECK(Vector2EditorRound(2.5) == 2);
	CHECK(Vector2EditorRound(-3.5) == -4);
	REQUIRE(PanVector2Pad(view, {80, 40}, {0, 0, 160, 160}));
	CHECK(view.MinimumX == -2);
	CHECK(view.MinimumY == -.5);
	REQUIRE(ZoomVector2Pad(view, 1000));
	CHECK(view.MinimumX == -101);
	CHECK(view.MaximumX == 99);
	CHECK(view.MinimumY == -99.5);
	CHECK(view.MaximumY == 100.5);
	REQUIRE(ZoomVector2Pad(view, -1000));
	CHECK(view.MinimumX == -2);
	CHECK(view.MaximumX == 0);
	REQUIRE(FocusVector2Pad(view, {8, -4}));
	CHECK(view.MinimumX == 7);
	CHECK(view.MaximumY == -3);
	const auto before = value;
	CHECK_FALSE(Vector2PadCoordinate(view, {0, 0, 0, 160}, {0, 0}, false, value));
	CHECK(value == before);
	CHECK_FALSE(FocusVector2Pad(view, {std::numeric_limits<double>::infinity(), 0}));
}

TEST_CASE(
	"source preview overlay applies project factor before downward drag and ordered snapping",
	"[studio][vector2_editor]"
) {
	Vector2 position;
	REQUIRE(Vector2OverlayPosition({.5, .25}, {80, 40}, {3, 7}, {10, 20}, 2, position));
	CHECK(position == Vector2{96, 54});
	Vector2PreviewSnap snap;
	snap.ShowGrid = true;
	snap.GridSize = {2, 4};
	snap.ShowRulers = true;
	std::array guides{
		PreviewRulerGuide{PreviewRulerAxis::Vertical, 6.5},
		PreviewRulerGuide{PreviewRulerAxis::Horizontal, -2.5}
	};
	snap.Guides = guides;
	Vector2 value;
	REQUIRE(Vector2OverlayDrag({.5, .25}, {80, -40}, {80, 40}, 2, {}, false, value));
	CHECK(value == Vector2{1, -.25});
	value = {6.1, -3.1};
	REQUIRE(SnapVector2Preview(value, snap, 4, true));
	CHECK(value == Vector2{6, -2});
	CHECK(HitVector2Overlay({9, 9}, {0, 0}, 1, {20, 20}, .01, 1));
	CHECK_FALSE(HitVector2Overlay({9, 9}, {0, 0}, 0, {20, 20}, 1, 1));
	CHECK(HitVector2Overlay({8, 0}, {0, 0}, 0, {0, 0}, 1, .1));
	std::vector<PreviewRulerGuide> oversized(Limits::MaximumArrayElements + 1);
	snap.Guides = oversized;
	const auto before = value;
	CHECK_FALSE(SnapVector2Preview(value, snap, 1, false));
	CHECK(value == before);
}

TEST_CASE(
	"sprite admission reserves retained and simultaneous replacement payloads", "[studio][vector2_editor]"
) {
	CHECK(ReserveVector2Sprite(64, 64, 3, 256));
	CHECK_FALSE(ReserveVector2Sprite(65, 64, 3, 256));
	CHECK_FALSE(ReserveVector2Sprite(std::numeric_limits<size_t>::max(), 64, 3));
	CHECK_FALSE(ReserveVector2Sprite(0, std::numeric_limits<size_t>::max(), 3));
	CHECK(ReserveVector2Sprite(0, 128 * 128 * 4, 4));
}

TEST_CASE(
	"Vector2 pad host gesture edits both components with one persisted undo step", "[studio][vector2_editor]"
) {
	auto doc = Graph();
	nodegraph::Graph canvasGraph;
	ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(LoadImageGraphCanvas(doc, canvasGraph, ids, error));
	nodegraph::Canvas canvas;
	ImageGraphHistory history;
	Vector2Panel panel;
	uint64_t revision = 1;
	panel.Attach(canvas, doc, ids, history, [&] { ++revision; });
	panel.Refresh(doc, {}, revision, 1);
	REQUIRE(panel.Presentation("point"));
	const auto node = canvasGraph.Nodes().front();
	CHECK(canvas.Signals.MeasureBody(node).Height == 160);
	nodegraph::BodyFrame frame;
	frame.Width = 160;
	frame.Height = 160;
	frame.Hovered = true;
	frame.LeftPressed = true;
	frame.LeftDown = true;
	frame.MouseX = 120;
	frame.MouseY = 40;
	const auto before = doc;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	CHECK_FALSE(history.CanUndo());
	frame.LeftPressed = false;
	frame.MouseX = 140;
	frame.MouseY = 20;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	const auto after = doc;
	frame.LeftDown = false;
	frame.LeftReleased = true;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	REQUIRE(history.Undo(doc));
	CHECK(doc == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(doc));
	CHECK(doc == after);
	Diagnostic diagnostic;
	const auto serialized = Write(doc);
	Document loaded;
	REQUIRE(Read(serialized, loaded, diagnostic) == Status::Ok);
	CHECK(loaded == after);
	Plan plan;
	REQUIRE(Compile(loaded, plan, diagnostic) == Status::Ok);
	ImageGraphPreviewValue preview;
	REQUIRE(EvaluateImageGraphPreview(loaded, plan, "point-value", {}, preview, diagnostic) == Status::Ok);
	CHECK(std::get<Vector2>(std::get<EvaluatedValue>(preview).Data) == Vector2{.75, .75});
	canvasGraph.Find(node.Id)->X += 20;
	Document canvasSaved;
	REQUIRE(SaveImageGraphCanvas(canvasGraph, doc, ids, canvasSaved, error));
	CHECK(canvasSaved.Nodes.front().Values == doc.Nodes.front().Values);
	const auto unchanged = doc;
	CHECK_FALSE(SetImageGraphVector2Coordinates(
		doc, "point", {2, std::numeric_limits<double>::infinity()}, diagnostic
	));
	CHECK(doc == unchanged);
	const auto oldRevision = revision;
	frame.LeftReleased = false;
	frame.Wheel = 2;
	frame.MiddlePressed = false;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	CHECK(doc == unchanged);
	CHECK(revision == oldRevision);
}

TEST_CASE(
	"Vector2 host body follows actual linked controls and refuses oversized sprites",
	"[studio][vector2_editor]"
) {
	auto doc = Graph();
	doc.Nodes.push_back({"size", "pc.solid", "", {}, {{"dimension", Vector2{129, 1}}}});
	doc.Nodes.front().Values.push_back({Input(7), int64_t{2}});
	const auto *entry = FindCatalogueEntry("pc.solid");
	REQUIRE(entry);
	REQUIRE_FALSE(entry->Outputs.empty());
	doc.Links.push_back({"size", std::string(entry->Outputs.front().Id), "point", Input(9)});
	Vector2Panel panel;
	EvaluationRequest request;
	request.MaximumImageDimension = 128;
	panel.Refresh(doc, request, 1, 1);
	CHECK_FALSE(panel.Presentation("point"));
	CHECK(panel.Diagnostic().Code == Status::LimitExceeded);
}

TEST_CASE(
	"coordinate gestures key animated axes at the selected frame without rewriting base values",
	"[studio][vector2_editor]"
) {
	auto doc = Graph();
	doc.Keyframes = {{"point", "x", 0, 0.0, "linear"}, {"point", "x", 10, 10.0, "linear"}};
	Diagnostic error;
	const auto before = doc;
	REQUIRE(SetImageGraphVector2CoordinatesAtFrame(doc, "point", {3, 8}, 5, 0, error));
	CHECK(std::get<double>(doc.Nodes.front().Values.front().Data) == 0);
	CHECK(doc.Keyframes.size() == 3);
	Vector2Presentation resolved;
	EvaluationRequest request;
	request.Tick = 5;
	REQUIRE(ResolveVector2Presentation(doc, "point", request, resolved, error) == Status::Ok);
	CHECK(resolved.X == 3);
	CHECK(resolved.Y == 8);
	const auto after = doc;
	CHECK_FALSE(SetImageGraphVector2CoordinatesAtFrame(doc, "point", {7, 2}, 5, 1, error));
	CHECK(doc == after);
	ImageGraphHistory history;
	history.Record(before, after);
	REQUIRE(history.Undo(doc));
	CHECK(doc == before);
	REQUIRE(history.Redo(doc));
	CHECK(doc == after);
	Document loaded;
	REQUIRE(Read(Write(doc), loaded, error) == Status::Ok);
	REQUIRE(ResolveVector2Presentation(loaded, "point", request, resolved, error) == Status::Ok);
	CHECK(resolved.X == 3);
	CHECK(resolved.Y == 8);
}

TEST_CASE(
	"coordinate body protects batched arrays and pins the gesture frame during playback",
	"[studio][vector2_editor]"
) {
	auto doc = Graph();
	nodegraph::Graph graph;
	ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(LoadImageGraphCanvas(doc, graph, ids, error));
	nodegraph::Canvas canvas;
	ImageGraphHistory history;
	Vector2Panel panel;
	uint64_t revision = 1;
	panel.Attach(canvas, doc, ids, history, [&] { ++revision; });
	doc.Nodes.front().Values.front().Data = ArrayValue{ValueType::Scalar, {1.0, 2.0}};
	panel.Refresh(doc, {}, revision, 1);
	const auto node = graph.Nodes().front();
	CHECK(canvas.Signals.MeasureBody(node).Height == 80);
	nodegraph::BodyFrame frame;
	frame.Width = 160;
	frame.Height = 160;
	frame.Hovered = true;
	frame.LeftDown = true;
	frame.LeftPressed = true;
	frame.MouseX = 120;
	frame.MouseY = 40;
	const auto batch = doc;
	CHECK_FALSE(canvas.Signals.InputBody(node, frame));
	CHECK(doc == batch);
	doc = Graph();
	doc.Keyframes = {{"point", "x", 0, 0.0, "linear"}, {"point", "x", 10, 10.0, "linear"}};
	EvaluationRequest request;
	request.Tick = 5;
	panel.Refresh(doc, request, ++revision, 1);
	REQUIRE(canvas.Signals.InputBody(node, frame));
	request.Tick = 6;
	panel.Refresh(doc, request, revision, 1);
	frame.LeftPressed = false;
	frame.MouseX = 140;
	frame.MouseY = 20;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	frame.LeftDown = false;
	frame.LeftReleased = true;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	CHECK(std::none_of(doc.Keyframes.begin(), doc.Keyframes.end(), [](const auto &key) {
		return key.Tick == 6;
	}));
	const auto key = std::find_if(doc.Keyframes.begin(), doc.Keyframes.end(), [](const auto &key) {
		return key.Tick == 5;
	});
	REQUIRE(key != doc.Keyframes.end());
	CHECK(std::get<double>(key->Data) == .75);
	REQUIRE(history.Undo(doc));
	CHECK(doc.Keyframes.size() == 2);
}

TEST_CASE("authored preview grid and ordered guides validate persist and undo", "[studio][vector2_editor]") {
	auto doc = Graph();
	const auto before = doc;
	ProjectSettings project;
	project.PreviewGrid = {true, true, {8, 0}};
	project.PreviewRulers = {{PreviewRulerAxis::Vertical, 6.5}, {PreviewRulerAxis::Horizontal, -2.5}};
	project.ShowPreviewRulers = true;
	Diagnostic error;
	REQUIRE(SetImageGraphProjectSettings(doc, project, error));
	CHECK(doc.FormatVersion == 9);
	const auto after = doc;
	Document loaded;
	REQUIRE(Read(Write(doc), loaded, error) == Status::Ok);
	CHECK(loaded == after);
	Vector2PreviewSnap snap{
		project.PreviewGrid.Show,
		project.PreviewGrid.Snap,
		project.PreviewGrid.Size,
		project.ShowPreviewRulers,
		loaded.Project->PreviewRulers
	};
	Vector2 coordinate{5, -2};
	REQUIRE(SnapVector2Preview(coordinate, snap, 1, false));
	CHECK(coordinate == Vector2{6.5, -2.5});
	ImageGraphHistory history;
	history.Record(before, after);
	REQUIRE(history.Undo(doc));
	CHECK(doc == before);
	REQUIRE(history.Redo(doc));
	CHECK(doc == after);
	project.PreviewGrid.Size.X = -1;
	CHECK_FALSE(SetImageGraphProjectSettings(doc, project, error));
	CHECK(doc == after);
	project.PreviewGrid.Size.X = 8;
	project.PreviewRulers[0].Axis = static_cast<PreviewRulerAxis>(99);
	CHECK_FALSE(SetImageGraphProjectSettings(doc, project, error));
	CHECK(doc == after);
	project.PreviewRulers.assign(Limits::MaximumArrayElements + 1, {});
	CHECK_FALSE(SetImageGraphProjectSettings(doc, project, error));
	CHECK(doc == after);
}

TEST_CASE(
	"resolved sprite presentation reuses idle pixels and preserves them after a refused replacement",
	"[studio][vector2_editor]"
) {
	auto doc = Graph();
	doc.Nodes.front().Values.push_back({"gizmo_style", EnumValue{2}});
	doc.Nodes.push_back(
		{"sprite",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{3}}, {"colour", Colour{12, 34, 56, 255}}}}
	);
	doc.Links.push_back({"sprite", "image", "point", "gizmo_sprite"});
	Vector2Panel panel;
	EvaluationRequest request;
	request.MaximumImageDimension = 128;
	panel.Refresh(doc, request, 1, 1);
	const auto *value = panel.Presentation("point");
	REQUIRE(value);
	REQUIRE(value->Sprite);
	const auto *pixels = value->Sprite->Pixels.data();
	panel.Refresh(doc, request, 1, 1);
	REQUIRE(panel.Presentation("point"));
	CHECK(panel.Presentation("point")->Sprite->Pixels.data() == pixels);
	doc.Nodes[1].Values[0].Data = int64_t{129};
	panel.Refresh(doc, request, 2, 1);
	CHECK_FALSE(panel.Presentation("point"));
	CHECK(panel.Diagnostic().Code == Status::LimitExceeded);
	CHECK(value->Sprite->Pixels.data() == pixels);
	panel.Refresh(doc, request, 2, 1);
	CHECK(panel.Diagnostic().Code == Status::LimitExceeded);
}

TEST_CASE(
	"signed fractional coordinate gestures retain exact keys through reload and undo",
	"[studio][vector2_editor]"
) {
	auto doc = Graph();
	doc.Keyframes = {{"point", "x", 0, 0.0, "linear"}, {"point", "x", 10, 10.0, "linear"}};
	nodegraph::Graph graph;
	ImageGraphCanvasIds ids;
	std::string canvasError;
	REQUIRE(LoadImageGraphCanvas(doc, graph, ids, canvasError));
	nodegraph::Canvas canvas;
	ImageGraphHistory history;
	Vector2Panel panel;
	uint64_t revision = 1;
	panel.Attach(canvas, doc, ids, history, [&] { ++revision; });
	ImageGraphPlayback playback;
	REQUIRE(SeekImageGraphAuthorFrame(playback, -5.25, true, true));
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, GetImageGraphFrame(playback)));
	panel.Refresh(doc, request, revision, 1);
	const auto before = doc;
	const auto node = graph.Nodes().front();
	nodegraph::BodyFrame frame;
	frame.Width = 160;
	frame.Height = 160;
	frame.Hovered = frame.LeftDown = frame.LeftPressed = true;
	frame.MouseX = 120;
	frame.MouseY = 40;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	REQUIRE(SeekImageGraphAuthorFrame(playback, 5.25, true, true));
	REQUIRE(SetFrameTime(request, GetImageGraphFrame(playback)));
	panel.Refresh(doc, request, revision, 1);
	frame.LeftPressed = false;
	frame.MouseX = 140;
	frame.MouseY = 20;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	frame.LeftDown = false;
	frame.LeftReleased = true;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	REQUIRE(doc.Keyframes.size() == 3);
	const auto &key = doc.Keyframes.back();
	CHECK(GetFrameTime(key) == FrameTime{5, .25, true});
	CHECK(std::get<double>(key.Data) == .75);
	CHECK(std::get<double>(doc.Nodes.front().Values.front().Data) == 0);
	CHECK(doc.FormatVersion == 9);
	const auto authored = doc;
	REQUIRE(history.Undo(doc));
	CHECK(doc == before);
	REQUIRE(history.Redo(doc));
	CHECK(doc == authored);
	Diagnostic error;
	Document loaded;
	REQUIRE(Read(Write(doc), loaded, error) == Status::Ok);
	CHECK(loaded == authored);
	REQUIRE(SetFrameTime(request, FrameTime{5, .25, true}));
	Vector2Presentation presentation;
	REQUIRE(ResolveVector2Presentation(loaded, "point", request, presentation, error) == Status::Ok);
	CHECK(presentation.X == .75);
	REQUIRE(SetImageGraphVector2CoordinatesAtFrame(loaded, "point", {4, 3}, 5, .25, error, true));
	CHECK(loaded.Keyframes.size() == 3);
	CHECK(std::get<double>(loaded.Keyframes.back().Data) == 4);
	REQUIRE(SetImageGraphVector2CoordinatesAtFrame(loaded, "point", {8, 3}, 5, .25, error));
	CHECK(loaded.Keyframes.size() == 4);
	CHECK(GetFrameTime(loaded.Keyframes.back()) == FrameTime{5, .25, false});
}

TEST_CASE(
	"coordinate gestures preserve an existing key kind and default new keys to Normal",
	"[studio][vector2_editor]"
) {
	auto doc = Graph();
	doc.FormatVersion = 9;
	doc.Keyframes = {{"point", "x", 1, 0.0, "linear"}, {"point", "x", 1, 2.0, "linear"}};
	REQUIRE(SetFrameTime(doc.Keyframes[0], {1, .25, true}));
	REQUIRE(SetFrameTime(doc.Keyframes[1], {1, .25, false}));
	doc.Keyframes[0].Kind = KeyframeKind::Adder;
	nodegraph::Graph graph;
	ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(LoadImageGraphCanvas(doc, graph, ids, error));
	nodegraph::Canvas canvas;
	ImageGraphHistory history;
	Vector2Panel panel;
	uint64_t revision = 1;
	panel.Attach(canvas, doc, ids, history, [&] { ++revision; });
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {1, .25, true}));
	panel.Refresh(doc, request, revision, 1);
	const auto before = doc;
	nodegraph::BodyFrame frame;
	frame.Width = frame.Height = 160;
	frame.Hovered = frame.LeftDown = frame.LeftPressed = true;
	frame.MouseX = 120;
	frame.MouseY = 40;
	const auto node = graph.Nodes().front();
	REQUIRE(canvas.Signals.InputBody(node, frame));
	frame.LeftDown = frame.LeftPressed = false;
	frame.LeftReleased = true;
	REQUIRE(canvas.Signals.InputBody(node, frame));
	CHECK(doc.Keyframes[0].Kind == KeyframeKind::Adder);
	CHECK(doc.Keyframes[1] == before.Keyframes[1]);
	CHECK(std::get<double>(doc.Keyframes[0].Data) == .5);
	const auto after = doc;
	REQUIRE(history.Undo(doc));
	CHECK(doc == before);
	REQUIRE(history.Redo(doc));
	CHECK(doc == after);
	Diagnostic diagnostic;
	REQUIRE(SetImageGraphVector2CoordinatesAtFrame(doc, "point", {3, 4}, 2, .25, diagnostic, true));
	CHECK(doc.Keyframes.back().Kind == KeyframeKind::Normal);
	Document loaded;
	REQUIRE(Read(Write(doc), loaded, diagnostic) == Status::Ok);
	CHECK(loaded == doc);
}
