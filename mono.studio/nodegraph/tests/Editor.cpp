#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Registry.hpp>
#include <nodegraph/Types.hpp>
#include <vector>

namespace {
	struct HeadlessContext {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Value = ImGui::CreateContext();

		HeadlessContext() {
			ImGui::SetCurrentContext(Value);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {640, 480};
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~HeadlessContext() {
			ImGui::SetCurrentContext(Value);
			ImGui::DestroyContext(Value);
			ImGui::SetCurrentContext(Previous);
		}
	};

	ImVec2 DrawFrame(nodegraph::Canvas &canvas, nodegraph::Graph &graph) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({600, 440});
		ImGui::Begin(
			"nodegraph-test",
			nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
		return origin;
	}

	void RegisterClickNodeType(const char *id, bool widgetsAndPorts = false) {
		nodegraph::NodeType type;
		type.Id = id;
		type.Title = "Click target";
		if (widgetsAndPorts) {
			type.Inputs = {nodegraph::Port("In", "data.NUMBER")};
			type.Outputs = {nodegraph::Port("Out", "data.NUMBER")};
			type.Widgets = {nodegraph::Number("amount", "Amount", 0.0)};
		}
		nodegraph::NodeTypes::Register(type);
	}
}

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

TEST_CASE(
	"node click hooks can consume body presses without changing selection or moving nodes",
	"[nodegraph][click]"
) {
	HeadlessContext context;
	RegisterClickNodeType("fixture.click-hook");
	nodegraph::Graph graph;
	const auto first = graph.Add("fixture.click-hook", 20, 20);
	const auto second = graph.Add("fixture.click-hook", 240, 20);
	nodegraph::Canvas canvas;
	canvas.Select(second);
	std::vector<nodegraph::NodeId> queued;
	int attempts = 0;
	canvas.Signals.ClickNode = [&](nodegraph::NodeId id) {
		++attempts;
		queued.push_back(id);
		return true;
	};
	ImVec2 origin = DrawFrame(canvas, graph);
	const auto firstLayout = nodegraph::LayoutOf(*graph.Find(first));
	const float firstX = origin.x + 20 + firstLayout.Width * 0.5f;
	const float firstY = origin.y + 20 + firstLayout.Height * 0.5f;
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(firstX, firstY);
	DrawFrame(canvas, graph);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame(canvas, graph);
	REQUIRE(queued == std::vector<nodegraph::NodeId>{first});
	const float originalX = graph.Find(first)->X;
	const float originalY = graph.Find(first)->Y;
	io.AddMousePosEvent(firstX + 80, firstY + 60);
	for (int frame = 0; frame < 12; ++frame) {
		io.DeltaTime = 0.02f;
		DrawFrame(canvas, graph);
	}
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame(canvas, graph);
	CHECK(attempts == 1);
	CHECK(graph.Find(first)->X == originalX);
	CHECK(graph.Find(first)->Y == originalY);
	CHECK(canvas.Selection() == std::vector<nodegraph::NodeId>{second});

	canvas.Signals.ClickNode = [&](nodegraph::NodeId) {
		++attempts;
		return false;
	};
	const float secondX = origin.x + 240 + firstLayout.Width * 0.5f;
	const float secondY = origin.y + 20 + firstLayout.Height * 0.5f;
	io.AddMousePosEvent(secondX, secondY);
	DrawFrame(canvas, graph);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame(canvas, graph);
	CHECK(canvas.Selection() == std::vector<nodegraph::NodeId>{second});
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame(canvas, graph);
	CHECK(attempts == 2);

	io.AddKeyEvent(ImGuiMod_Shift, true);
	io.AddMousePosEvent(firstX, firstY);
	DrawFrame(canvas, graph);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame(canvas, graph);
	CHECK(attempts == 2);
	CHECK(canvas.Selection() == (std::vector<nodegraph::NodeId>{second, first}));
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame(canvas, graph);
	io.AddKeyEvent(ImGuiMod_Shift, false);
	DrawFrame(canvas, graph);
}

TEST_CASE("node click hooks leave port and widget presses to the canvas", "[nodegraph][click]") {
	HeadlessContext context;
	RegisterClickNodeType("fixture.click-priority", true);
	nodegraph::Graph graph;
	const auto node = graph.Add("fixture.click-priority", 20, 20);
	nodegraph::Canvas canvas;
	int attempts = 0;
	canvas.Signals.ClickNode = [&](nodegraph::NodeId) {
		++attempts;
		return true;
	};
	ImVec2 origin = DrawFrame(canvas, graph);
	const auto layout = nodegraph::LayoutOf(*graph.Find(node));
	const auto &widget = layout.Widgets.front();
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(
		origin.x + 20 + widget.X + widget.Width * 0.5f, origin.y + 20 + widget.Y + widget.Height * 0.5f
	);
	DrawFrame(canvas, graph);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame(canvas, graph);
	CHECK(attempts == 0);
	CHECK(canvas.Selection().empty());
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame(canvas, graph);

	const auto &port = layout.Ports.front();
	io.AddMousePosEvent(origin.x + 20 + port.X, origin.y + 20 + port.Y);
	DrawFrame(canvas, graph);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame(canvas, graph);
	CHECK(attempts == 0);
	CHECK(canvas.Selection().empty());
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame(canvas, graph);
}

TEST_CASE("node marks highlight nodes without replacing selection", "[nodegraph][mark]") {
	HeadlessContext context;
	RegisterClickNodeType("fixture.node-mark");
	nodegraph::Graph graph;
	const auto marked = graph.Add("fixture.node-mark", 20, 20);
	const auto selected = graph.Add("fixture.node-mark", 240, 20);
	nodegraph::Canvas canvas;
	canvas.Look.NodeSelected = 0xFF17A35Bu;
	canvas.Select(selected);
	const auto highlightedVertices = [&] {
		int count = 0;
		const ImDrawData *data = ImGui::GetDrawData();
		for (int list = 0; list < data->CmdListsCount; ++list)
			for (const ImDrawVert &vertex : data->CmdLists[list]->VtxBuffer)
				if (vertex.col == canvas.Look.NodeSelected) ++count;
		return count;
	};
	DrawFrame(canvas, graph);
	const int selectedOnly = highlightedVertices();
	REQUIRE(selectedOnly > 0);
	canvas.MarkNodes({marked, marked});
	CHECK(canvas.Selection() == std::vector<nodegraph::NodeId>{selected});
	DrawFrame(canvas, graph);
	CHECK(highlightedVertices() > selectedOnly);
	CHECK(canvas.Selection() == std::vector<nodegraph::NodeId>{selected});
}

TEST_CASE("link drag dims refused sockets and paints corner port hints", "[nodegraph][drag]") {
	const bool reverse = GENERATE(false, true);
	HeadlessContext context;
	nodegraph::DataType number;
	number.Id = "fixture.drag.number";
	number.Tint = {1, 0, 0, 1};
	nodegraph::DataTypes::Register(number);
	nodegraph::DataType other;
	other.Id = "fixture.drag.other";
	other.Tint = {0, 1, 0, 1};
	nodegraph::DataTypes::Register(other);
	nodegraph::DataType either;
	either.Id = "fixture.drag.union";
	either.Tint = {0, 0, 1, 1};
	either.Members = {number.Id, other.Id};
	nodegraph::DataTypes::Register(either);
	nodegraph::NodeType type;
	type.Id = "fixture.drag.node";
	type.Title = "Drag";
	type.Inputs = {nodegraph::Port("good", either.Id), nodegraph::Port("bad", other.Id)};
	type.Outputs = {nodegraph::Port("out", number.Id)};
	nodegraph::NodeTypes::Register(type);
	nodegraph::Graph graph;
	const auto source = graph.Add(type.Id, 20, 100);
	const auto target = graph.Add(type.Id, 280, 100);
	nodegraph::Canvas canvas;
	const auto origin = DrawFrame(canvas, graph);
	const auto sourceLayout = nodegraph::LayoutOf(*graph.Find(source));
	const auto socket = [](const auto &layout, const std::string &name) {
		const auto found = std::find_if(layout.Ports.begin(), layout.Ports.end(), [&](const auto &port) {
			return port.Name == name;
		});
		REQUIRE(found != layout.Ports.end());
		return *found;
	};
	const auto output = socket(sourceLayout, "out");
	const auto targetLayout = nodegraph::LayoutOf(*graph.Find(target));
	const ImVec2 sourceAt(origin.x + 20 + output.X, origin.y + 100 + output.Y);
	const ImVec2 targetAt(
		origin.x + 280 + socket(targetLayout, "good").X, origin.y + 100 + socket(targetLayout, "good").Y
	);
	const auto anchor = reverse ? targetAt : sourceAt;
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(anchor.x, anchor.y);
	DrawFrame(canvas, graph);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame(canvas, graph);
	const auto layout = nodegraph::LayoutOf(*graph.Find(target));
	const auto hasTint = [&](const nodegraph::PlacedPort &port, ImU32 tint) {
		const ImVec2 at(origin.x + 280 + port.X, origin.y + 100 + port.Y);
		const auto *data = ImGui::GetDrawData();
		for (int list = 0; list < data->CmdListsCount; ++list)
			for (const auto &vertex : data->CmdLists[list]->VtxBuffer)
				if (vertex.col == tint && std::abs(vertex.pos.x - at.x) < 6 &&
					std::abs(vertex.pos.y - at.y) < 6)
					return true;
		return false;
	};
	CHECK(hasTint(socket(layout, "good"), IM_COL32(0, 0, 255, 255)));
	CHECK(hasTint(socket(layout, "bad"), IM_COL32(0, 255, 0, 51)));
	bool cornerText = false;
	const auto *data = ImGui::GetDrawData();
	for (int list = 0; list < data->CmdListsCount; ++list)
		for (const auto &vertex : data->CmdLists[list]->VtxBuffer)
			if (vertex.col == IM_COL32(255, 255, 255, 255) && vertex.pos.x > origin.x + 200 &&
				vertex.pos.y < origin.y + 70)
				cornerText = true;
	CHECK(cornerText);
	const auto drop = reverse ? sourceAt : targetAt;
	io.AddMousePosEvent(drop.x, drop.y);
	DrawFrame(canvas, graph);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame(canvas, graph);
	REQUIRE(graph.Links().size() == 1);
	CHECK(graph.Links().front().ToPort == "good");
}
