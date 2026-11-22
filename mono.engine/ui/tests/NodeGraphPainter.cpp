#include <engine/testing/Suite.hpp>
#include <engine/ui/NodeGraphPainter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

TEST_SUITE_ID("engine.ui.nodegraphpainter")
TEST_DEPENDS("engine.angview.view")

namespace {
	struct Frames {
		Frames() {
			ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Frames() {
			ImGui::DestroyContext();
		}

		// One frame with the pointer at `mouse` and the left button as given.
		ImDrawList *Run(engine::angview::View &view, ImVec2 mouse, bool down) {
			ImGuiIO &io = ImGui::GetIO();
			io.AddMousePosEvent(mouse.x, mouse.y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0.0f, 0.0f});
			ImGui::SetNextWindowSize({800.0f, 600.0f});
			ImGui::Begin("graph", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
			Origin = ImGui::GetCursorScreenPos();
			engine::ui::NodeGraphCanvas("canvas", view);
			ImDrawList *list = ImGui::GetWindowDrawList();
			Vertices = list->VtxBuffer.Size;
			ImGui::End();
			ImGui::EndFrame();
			return list;
		}

		ImVec2 Origin{};
		int Vertices = 0;
	};

	engine::ang::Registry Types() {
		engine::ang::Registry registry = engine::ang::Registry::WithScalars();
		registry.DefineNode({.Id = "number", .Outputs = {{.Name = "value", .Type = "number"}}});
		return registry;
	}
}

TEST_CASE("the ImGui canvas forwards a drag to the view and paints it", "[ui][headless][nodegraph]") {
	const engine::ang::Registry registry = Types();
	engine::ang::Graph graph;
	REQUIRE(
		graph.Apply(engine::ang::edit::AddNode{{.Id = "n", .Def = "number", .Position = {100.0f, 100.0f}}}).Ok
	);
	engine::angview::View view(graph, registry, engine::ui::NodeGraphThemeFromPalette());
	Frames frames;
	frames.Run(view, {0.0f, 0.0f}, false);
	const ImVec2 at{frames.Origin.x + 150.0f, frames.Origin.y + 120.0f};
	frames.Run(view, at, false);
	frames.Run(view, at, true);
	CHECK(view.Dragging() == engine::angview::DragKind::Nodes);
	frames.Run(view, {at.x + 40.0f, at.y + 10.0f}, true);
	frames.Run(view, {at.x + 40.0f, at.y + 10.0f}, false);
	CHECK(graph.FindNode("n")->Position.X == 140.0f);
	CHECK(graph.FindNode("n")->Position.Y == 110.0f);
	CHECK(frames.Vertices > 0);
}

TEST_CASE("every draw kind reaches the ImGui list", "[ui][headless][nodegraph]") {
	Frames frames;
	ImGui::NewFrame();
	ImGui::Begin("paint");
	ImDrawList &out = *ImGui::GetWindowDrawList();
	const int before = out.VtxBuffer.Size;
	using engine::angview::DrawKind;
	const engine::angview::Rgba white{1.0f, 1.0f, 1.0f, 1.0f};
	int resolved = 0;
	engine::ui::PaintNodeGraph(
		{
			{.Kind = DrawKind::PushClip, .A = {0.0f, 0.0f}, .B = {400.0f, 400.0f}},
			{.Kind = DrawKind::Rect, .A = {0.0f, 0.0f}, .B = {50.0f, 50.0f}, .Fill = white, .Stroke = white},
			{.Kind = DrawKind::Line, .A = {0.0f, 0.0f}, .B = {50.0f, 50.0f}, .Stroke = white},
			{.Kind = DrawKind::Bezier,
			 .A = {0.0f, 0.0f},
			 .B = {90.0f, 50.0f},
			 .C = {40.0f, 0.0f},
			 .D = {50.0f, 50.0f},
			 .Stroke = white},
			{.Kind = DrawKind::Circle, .A = {20.0f, 20.0f}, .Fill = white, .Radius = 5.0f},
			{.Kind = DrawKind::Diamond, .A = {30.0f, 30.0f}, .Fill = white, .Radius = 5.0f},
			{.Kind = DrawKind::Text, .A = {10.0f, 10.0f}, .B = {40.0f, 0.0f}, .Fill = white, .Text = "Add"},
			{.Kind = DrawKind::Image, .A = {0.0f, 0.0f}, .B = {10.0f, 10.0f}, .Image = 7},
			{.Kind = DrawKind::PopClip},
		},
		out,
		{0.0f, 0.0f},
		[&](uint64_t handle) {
			resolved += handle == 7 ? 1 : 0;
			return ImTextureID{};
		}
	);
	CHECK(out.VtxBuffer.Size > before);
	CHECK(resolved == 1);
	ImGui::End();
	ImGui::EndFrame();
}
