#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Registry.hpp>

TEST_SUITE_ID("studio.nodegraph.editor")

TEST_CASE("host node body captures wheel and dragging before canvas gestures", "[nodegraph][body]") {
	struct Context {
		ImGuiContext *Value = ImGui::CreateContext();
		Context() {
			auto &io = ImGui::GetIO();
			io.DisplaySize = {640, 480};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Context() {
			ImGui::DestroyContext(Value);
		}
	} context;
	nodegraph::NodeType type;
	type.Id = "fixture.hostbody";
	type.Title = "Body";
	nodegraph::NodeTypes::Register(type);
	nodegraph::Graph graph;
	const auto id = graph.Add(type.Id, 0, 0);
	nodegraph::Canvas canvas;
	canvas.Signals.MeasureBody = [](const auto &) { return nodegraph::BodySize{160, 160}; };
	nodegraph::BodyFrame last;
	canvas.Signals.InputBody = [&](const auto &, const auto &frame) {
		last = frame;
		return frame.Hovered;
	};
	int painted = 0;
	canvas.Signals.DrawBody = [&](const auto &, const auto &frame) {
		++painted;
		CHECK(frame.Width >= 160);
	};
	auto frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({600, 440});
		ImGui::Begin(
			"body", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		);
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
	};
	frame();
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(last.X + 40, last.Y + 40);
	frame();
	REQUIRE(last.Hovered);
	const auto scale = canvas.Zoom();
	const auto x = graph.Find(id)->X, y = graph.Find(id)->Y;
	io.AddMouseWheelEvent(0, 2);
	frame();
	CHECK(last.Wheel == 2);
	CHECK(canvas.Zoom() == scale);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	frame();
	io.AddMousePosEvent(last.X + 90, last.Y + 90);
	frame();
	CHECK(graph.Find(id)->X == x);
	CHECK(graph.Find(id)->Y == y);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	frame();
	CHECK(painted >= 6);
	const auto normal = nodegraph::LayoutOf(*graph.Find(id));
	const auto body = nodegraph::LayoutOf(*graph.Find(id), {}, {160, 160});
	CHECK(body.BodyHeight == 160);
	CHECK(body.Height > normal.Height);
	CHECK(body.BodyTop + body.BodyHeight <= body.Height);
	graph.Find(id)->Collapsed = true;
	CHECK(nodegraph::LayoutOf(*graph.Find(id), {}, {160, 160}).BodyHeight == 0);
}
