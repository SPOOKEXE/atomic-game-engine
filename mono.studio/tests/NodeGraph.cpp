// Studio's device-free checks for the generic nodegraph canvas and inspector.
// Their small node vocabulary is local to this suite; production graph editors
// register their own types.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Evaluate.hpp>
#include <nodegraph/Graph.hpp>
#include <nodegraph/Inspect.hpp>
#include <nodegraph/Registry.hpp>
#include <nodegraph/Serialize.hpp>
#include <nodegraph/Types.hpp>
#include <studio/RenderPipelineGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.nodegraph")

namespace {
	// Shared imgui context across all cases in this suite.
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

		void Reset() {
			// Clear any state that persists between cases.
			ImGui::SetCurrentContext(Handle);
			// A case that aborts its own drawing path must not make the next case
			// fail at `NewFrame` with an unrelated frame-lifetime assertion.
			if (GImGui->WithinFrameScope) {
				ImGui::EndFrame();
			}
		}

	  private:
		ImGuiContext *Handle = nullptr;
	};

	// Global shared context, constructed once for the suite.
	Context &SharedContext() {
		static Context ctx;
		return ctx;
	}

	// RAII guard that resets the shared context before each case.
	struct ContextGuard {
		ContextGuard() {
			SharedContext().Reset();
		}
	};

	// A picture and a height surface make the canvas and orbit inspector run
	// through the public registration path.
	void RegisterFieldFixture() {
		nodegraph::DataType field;
		field.Id = "test.FIELD";
		field.Label = "Field";
		field.Preview = [](const std::any &payload, nodegraph::PreviewImage &out) {
			const auto *surface = std::any_cast<nodegraph::Surface>(&payload);
			if (surface == nullptr || !surface->Valid()) {
				return false;
			}
			out.Side = surface->Side;
			out.Rgba.resize(static_cast<size_t>(out.Side) * out.Side * 4);
			for (size_t index = 0; index < surface->Heights.size(); index++) {
				const uint8_t grey = static_cast<uint8_t>(surface->Heights[index] * 255.0f);
				out.Rgba[index * 4 + 0] = grey;
				out.Rgba[index * 4 + 1] = grey;
				out.Rgba[index * 4 + 2] = grey;
				out.Rgba[index * 4 + 3] = 255;
			}
			return true;
		};
		field.Heights = [](const std::any &payload, nodegraph::Surface &out) {
			const auto *surface = std::any_cast<nodegraph::Surface>(&payload);
			if (surface == nullptr || !surface->Valid()) {
				return false;
			}
			out = *surface;
			return true;
		};
		nodegraph::DataTypes::Register(field);

		nodegraph::NodeType source;
		source.Id = "test.field";
		source.Title = "Field";
		source.Category = "Test";
		source.Outputs = {nodegraph::Port("Out", field.Id)};
		source.Evaluate = [](const nodegraph::Inputs &) {
			nodegraph::Surface surface;
			surface.Side = 8;
			surface.Heights.resize(64);
			for (size_t index = 0; index < surface.Heights.size(); index++) {
				surface.Heights[index] = static_cast<float>(index % surface.Side) / 7.0f;
			}
			return nodegraph::Outputs{{"Out", std::move(surface)}};
		};
		nodegraph::NodeTypes::Register(source);
	}
}

TEST_CASE("a hovered render node submits its retained preview texture", "[studio][nodegraph][preview]") {
	ContextGuard context_guard;
	studio::RegisterRenderPipelineNodeTypes();

	nodegraph::Graph graph;
	const nodegraph::NodeId shadow = graph.Add("render.pass.shadow", 60.0f, 60.0f);
	REQUIRE(shadow != nodegraph::NO_NODE);

	nodegraph::Evaluator evaluator;
	evaluator.Run(graph);
	nodegraph::Canvas canvas;
	canvas.Observe(&evaluator);
	void *const retainedTexture = reinterpret_cast<void *>(uintptr_t{0x1234});
	size_t previewRequests = 0;
	canvas.Images([&](uint64_t, const std::function<bool(nodegraph::PreviewImage &)> &) {
		previewRequests++;
		return retainedTexture;
	});

	for (int frame = 0; frame < 3; frame++) {
		ImGuiIO &io = ImGui::GetIO();
		// The point is inside the shadow node's preview rectangle. The first
		// frame establishes the window; the following frames exercise hover.
		io.AddMousePosEvent(180.0f, 210.0f);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
		ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f));
		if (ImGui::Begin(
				"render preview canvas",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoScrollbar
			)) {
			canvas.Draw(graph);
		}
		ImGui::End();
		ImGui::Render();
	}

	CHECK(previewRequests >= 2);
	bool submitted = false;
	const ImTextureID expected = reinterpret_cast<ImTextureID>(retainedTexture);
	const ImDrawData *drawData = ImGui::GetDrawData();
	REQUIRE(drawData != nullptr);
	for (int list = 0; list < drawData->CmdListsCount; list++) {
		for (const ImDrawCmd &command : drawData->CmdLists[list]->CmdBuffer) {
			// Font commands carry an ImTextureData that a device backend has not
			// uploaded in this device-free suite. Read only user texture commands,
			// whose direct id is exactly what the SDL GPU backend later receives.
			submitted =
				submitted || (command.TexRef._TexData == nullptr && command.TexRef._TexID == expected);
		}
	}
	CHECK(submitted);
}

TEST_CASE("a canvas node waits through the drag deadzone", "[studio][nodegraph][input]") {
	ContextGuard context_guard;
	RegisterFieldFixture();

	nodegraph::Graph graph;
	const nodegraph::NodeId node = graph.Add("test.field", 60.0f, 60.0f);
	REQUIRE(node != nodegraph::NO_NODE);
	nodegraph::Canvas canvas;
	int changed = 0;
	canvas.Signals.Changed = [&] { ++changed; };

	const auto frame = [&](ImVec2 mouse, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
		ImGui::Begin(
			"canvas deadzone",
			nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
		);
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
	};

	const ImVec2 pressed(100.0f, 100.0f);
	const ImVec2 moved(140.0f, 100.0f);
	frame(pressed, false);
	frame(pressed, true);
	frame(moved, true);
	REQUIRE(canvas.Selection() == std::vector<nodegraph::NodeId>{node});
	CHECK(graph.Find(node)->X == 60.0f);

	for (int frameIndex = 0; frameIndex < 10; frameIndex++) {
		frame(moved, true);
	}
	CHECK(graph.Find(node)->X == 100.0f);
	frame(moved, false);
	CHECK(changed == 1);
}

TEST_CASE("empty canvas shortcuts do not report an edit", "[studio][nodegraph][input]") {
	ContextGuard context_guard;
	nodegraph::Graph graph;
	nodegraph::Canvas canvas;
	int changed = 0;
	canvas.Signals.Changed = [&] { ++changed; };

	const auto frame = [&](ImGuiKey key, bool down, bool control) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddKeyEvent(ImGuiMod_Ctrl, control);
		io.AddKeyEvent(key, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
		ImGui::SetNextWindowFocus();
		ImGui::Begin("empty canvas shortcuts", nullptr, ImGuiWindowFlags_NoSavedSettings);
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
	};

	frame(ImGuiKey_Delete, false, false);
	frame(ImGuiKey_Delete, true, false);
	frame(ImGuiKey_Delete, false, false);
	CHECK(changed == 0);

	frame(ImGuiKey_V, true, true);
	frame(ImGuiKey_V, false, false);
	CHECK(changed == 0);
	CHECK(graph.Nodes().empty());

	RegisterFieldFixture();
	const auto node = graph.Add("test.field", 60.0f, 60.0f);
	REQUIRE(node != nodegraph::NO_NODE);
	canvas.Select(node);
	canvas.Copy(graph);
	frame(ImGuiKey_Delete, true, false);
	frame(ImGuiKey_Delete, false, false);
	CHECK(graph.Nodes().empty());
	CHECK(changed == 1);
	frame(ImGuiKey_C, true, true);
	frame(ImGuiKey_C, false, false);
	CHECK(canvas.CanPaste());
	frame(ImGuiKey_V, true, true);
	frame(ImGuiKey_V, false, false);
	CHECK(graph.Nodes().size() == 1);
	CHECK(changed == 2);
}

TEST_CASE("group frames only respond at their nodes' depth", "[studio][nodegraph][input]") {
	ContextGuard context_guard;
	RegisterFieldFixture();
	nodegraph::NodeType sink;
	sink.Id = "test.group.depth.sink";
	sink.Title = "Sink";
	sink.Inputs = {nodegraph::Port("In", "test.FIELD")};
	nodegraph::NodeTypes::Register(sink);
	nodegraph::Graph graph;
	const auto first = graph.Add("test.field", 100.0f, 100.0f);
	const auto second = graph.Add("test.field", 300.0f, 100.0f);
	const auto output = graph.Add(sink.Id, 700.0f, 300.0f);
	REQUIRE(graph.Connect(first, "Out", output, "In") == nodegraph::LinkResult::Made);
	const auto fold = graph.Compress({first, second}, 500.0f, 200.0f);
	REQUIRE(fold != nodegraph::NO_NODE);
	const auto group = graph.Group({first, second}, "Hidden", nodegraph::Colour::Hex(0x7C5CFF));
	REQUIRE(group != nodegraph::NO_GROUP);
	nodegraph::Canvas canvas;
	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(800.0f, 500.0f));
		ImGui::Begin("group depth", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
	};
	frame({120.0f, 78.0f}, false);
	frame({120.0f, 78.0f}, true);
	CHECK(canvas.SelectedGroup() == nodegraph::NO_GROUP);
	frame({120.0f, 78.0f}, false);
	canvas.Enter(graph, fold);
	const ImVec2 inside(8.0f + 60.0f * canvas.Zoom(), 8.0f + 10.0f * canvas.Zoom());
	frame(inside, false);
	frame(inside, true);
	CHECK(canvas.SelectedGroup() == group);
	frame(inside, false);
}

TEST_CASE("a mixed-depth group cannot move hidden members", "[studio][nodegraph][input]") {
	ContextGuard context_guard;
	RegisterFieldFixture();
	nodegraph::Graph graph;
	const auto hidden = graph.Add("test.field", 100.0f, 100.0f);
	const auto peer = graph.Add("test.field", 300.0f, 100.0f);
	const auto fold = graph.Compress({hidden, peer}, 500.0f, 200.0f);
	REQUIRE(fold != nodegraph::NO_NODE);
	const auto visible = graph.Add("test.field", 100.0f, 100.0f);
	const auto group = graph.Group({hidden, visible}, "Mixed", nodegraph::Colour::Hex(0x7C5CFF));
	REQUIRE(group != nodegraph::NO_GROUP);
	nodegraph::Canvas canvas;
	const auto frame = [&](ImVec2 mouse, bool down) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(800.0f, 500.0f));
		ImGui::Begin(
			"mixed group depth", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings
		);
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
	};
	frame({120.0f, 78.0f}, false);
	frame({120.0f, 78.0f}, true);
	for (int index = 0; index < 12; ++index)
		frame({160.0f, 78.0f}, true);
	frame({160.0f, 78.0f}, false);
	CHECK(graph.Find(visible)->X == 100.0f);
	CHECK(graph.Find(hidden)->X == 100.0f);
	canvas.Enter(graph, fold);
	const ImVec2 inside(8.0f + 60.0f * canvas.Zoom(), 8.0f + 10.0f * canvas.Zoom());
	frame(inside, false);
	frame(inside, true);
	CHECK(canvas.SelectedGroup() == nodegraph::NO_GROUP);
	frame(inside, false);
	canvas.Leave(graph);
	REQUIRE(graph.Expand(fold));
	canvas.Select(nodegraph::NO_NODE);
	canvas.Fit(graph);
	const ImVec2 reunited(8.0f + 60.0f * canvas.Zoom(), 8.0f + 10.0f * canvas.Zoom());
	frame(reunited, false);
	frame(reunited, true);
	CHECK(canvas.SelectedGroup() == group);
	frame(reunited, false);
}

TEST_CASE("folds with no external ports remain navigable", "[studio][nodegraph]") {
	ContextGuard context_guard;
	RegisterFieldFixture();
	nodegraph::Graph graph;
	const auto first = graph.Add("test.field", 100.0f, 100.0f);
	const auto second = graph.Add("test.field", 300.0f, 100.0f);
	const auto fold = graph.Compress({first, second}, 500.0f, 200.0f);
	REQUIRE(fold != nodegraph::NO_NODE);
	nodegraph::Graph restored;
	std::string error;
	REQUIRE(nodegraph::Load(nodegraph::Save(graph), restored, error));
	REQUIRE(restored.Find(fold) != nullptr);
	CHECK(restored.Find(fold)->Compressed());
	nodegraph::Canvas restoredCanvas;
	restoredCanvas.Enter(restored, fold);
	CHECK(restoredCanvas.Inside() == fold);
	restoredCanvas.Leave(restored);
	CHECK(restored.Expand(fold));
	nodegraph::Canvas canvas;
	canvas.Enter(graph, fold);
	CHECK(canvas.Inside() == fold);
	canvas.Leave(graph);
	REQUIRE(graph.Expand(fold));
	CHECK(graph.Find(first)->Owner == nodegraph::NO_NODE);
	CHECK(graph.Find(second)->Owner == nodegraph::NO_NODE);
}

TEST_CASE("disconnecting an unlinked canvas node does not report an edit", "[studio][nodegraph][input]") {
	ContextGuard context_guard;
	RegisterFieldFixture();
	nodegraph::NodeType sink;
	sink.Id = "test.field.sink";
	sink.Title = "Sink";
	sink.Category = "Test";
	sink.Inputs = {nodegraph::Port("In", "test.FIELD")};
	nodegraph::NodeTypes::Register(sink);

	nodegraph::Graph graph;
	const auto source = graph.Add("test.field", 60.0f, 60.0f);
	const auto target = graph.Add("test.field.sink", 300.0f, 60.0f);
	REQUIRE(source != nodegraph::NO_NODE);
	REQUIRE(target != nodegraph::NO_NODE);
	nodegraph::Canvas canvas;
	int changed = 0;
	canvas.Signals.Changed = [&] { ++changed; };

	const auto frame = [&](bool rightDown, bool focus) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(100.0f, 100.0f);
		io.AddMouseButtonEvent(ImGuiMouseButton_Right, rightDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
		if (focus) ImGui::SetNextWindowFocus();
		ImGui::Begin(
			"canvas disconnect",
			nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
		);
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
	};
	const auto disconnect = [&] {
		frame(false, true);
		frame(true, false);
		REQUIRE(canvas.Selection() == std::vector<nodegraph::NodeId>{source});
		REQUIRE_FALSE(GImGui->OpenPopupStack.empty());
		ImGuiWindow *const popup = GImGui->OpenPopupStack.back().Window;
		REQUIRE(popup != nullptr);
		const ImGuiID item = popup->GetID("Disconnect all");
		frame(false, false);
		ImGui::ActivateItemByID(item);
		frame(false, false);
	};

	disconnect();
	CHECK(graph.Links().empty());
	CHECK(changed == 0);
	REQUIRE(graph.Connect(source, "Out", target, "In") == nodegraph::LinkResult::Made);
	disconnect();
	CHECK(graph.Links().empty());
	CHECK(changed == 1);
}

TEST_CASE("the field mesh preview owns its drag before the host window", "[studio][nodegraph][input]") {
	ContextGuard context_guard;
	RegisterFieldFixture();

	nodegraph::Graph graph;
	const nodegraph::NodeId source = graph.Add("test.field", 0.0f, 0.0f);
	REQUIRE(source != nodegraph::NO_NODE);

	nodegraph::Evaluator evaluator;
	evaluator.RunToCompletion(graph);
	const nodegraph::Node *node = graph.Find(source);
	const nodegraph::NodeType *type = nodegraph::NodeTypes::Find("test.field");
	REQUIRE(node != nullptr);
	REQUIRE(type != nullptr);

	void *const previewTexture = reinterpret_cast<void *>(uintptr_t{0x1234});
	nodegraph::Inspection inspection;
	inspection.Node = node;
	inspection.Type = type;
	inspection.Graph = &graph;
	inspection.Runner = &evaluator;
	inspection.Images = [previewTexture](uint64_t, const auto &) { return previewTexture; };
	inspection.Orbit = [previewTexture](uint64_t, const auto &) { return previewTexture; };
	const nodegraph::InspectorFn *draw = nodegraph::Inspectors::For(inspection);
	REQUIRE(draw != nullptr);

	struct PreviewFrame {
		ImVec2 Position;
		float Yaw = 0.0f;
	};
	const auto frame = [&](ImVec2 mouse, bool down, bool place) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		if (place) {
			ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
			ImGui::SetNextWindowSize(ImVec2(420.0f, 620.0f));
		}
		ImGui::Begin(
			"Field inspector",
			nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoScrollbar
		);
		ImGuiStorage *state = ImGui::GetStateStorage();
		state->SetBool(ImGui::GetID("nodegraph.pin.three"), true);
		(*draw)(inspection);
		const PreviewFrame result{
			ImGui::GetWindowPos(), state->GetFloat(ImGui::GetID("nodegraph.pin.yaw"), 0.6f)
		};
		ImGui::End();
		ImGui::Render();
		return result;
	};

	const ImVec2 overPreview(200.0f, 200.0f);
	const PreviewFrame before = frame(overPreview, false, true);
	(void)frame(overPreview, false, false);
	(void)frame(overPreview, true, false);
	const PreviewFrame after = frame(ImVec2(140.0f, 200.0f), true, false);
	(void)frame(ImVec2(140.0f, 200.0f), false, false);

	CHECK(after.Position.x == before.Position.x);
	CHECK(after.Position.y == before.Position.y);
	CHECK(after.Yaw > before.Yaw);
}
