#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <imgui.h>
#include <nodegraph/Editor.hpp>
#include <string>
#include <studio/ImageGraph.hpp>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.pattern_groups")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	constexpr std::array<SurfaceFormat, 7> KIS_FORMATS{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};

	Document PatternGraph(int64_t kisDepth) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"hilbert",
			 "pc.hilbert",
			 "",
			 {0, 0},
			 {{"dimension", Vector2{8, 8}},
			  {"dimension_unit", EnumValue{0}},
			  {"iteration", int64_t{1}},
			  {"orientation", EnumValue{1}},
			  {"thickness", 2.},
			  {"bg_color", Colour{7, 8, 9, 17}},
			  {"path_color", Gradient{0, {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}}}}}},
			{"invert", "pc.invert", "", {180, 0}, {}},
			{"kis",
			 "pc.kisrhombille",
			 "",
			 {0, 180},
			 {{"dimension", Vector2{8, 8}},
			  {"dimension_unit", EnumValue{0}},
			  {"attribute_color_depth", EnumValue{kisDepth}},
			  {"position", Vector2{}},
			  {"position_unit", EnumValue{0}},
			  {"scale", Vector2{2, 2}},
			  {"scale_unit", EnumValue{0}},
			  {"grouping", EnumValue{0}},
			  {"color_1", Colour{17, 31, 47, 255}},
			  {"color_2", Colour{223, 199, 173, 255}}}}
		};
		document.Links = {{"hilbert", "surface_out", "invert", "surface_in"}};
		document.Outputs = {{"inverted", "invert", "surface_out"}, {"kis_output", "kis", "surface_out"}};
		return document;
	}

	struct CanvasInput {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		nodegraph::Graph Graph;
		nodegraph::Canvas Canvas;
		studio::ImageGraphCanvasIds Ids;
		bool Focused = false;
		CanvasInput() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {800, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~CanvasInput() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({740, 540});
			ImGui::SetNextWindowFocus();
			ImGui::Begin("Pattern canvas", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			Canvas.Draw(Graph);
			Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
			ImGui::End();
			ImGui::Render();
		}
		void GroupKey(bool ungroup) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(ImGuiMod_Ctrl, true);
			io.AddKeyEvent(ImGuiMod_Shift, ungroup);
			io.AddKeyEvent(ImGuiKey_G, true);
			Frame();
			io.AddKeyEvent(ImGuiKey_G, false);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			io.AddKeyEvent(ImGuiMod_Shift, false);
			Frame();
		}
	};

	Image EvaluateOutput(const Document &document, const std::string &output) {
		Diagnostic diagnostic;
		Plan plan;
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		Image image;
		REQUIRE(Evaluate(document, plan, output, {}, image, diagnostic) == Status::Ok);
		return image;
	}

	Document SaveRoundTrip(CanvasInput &ui, const Document &basis) {
		std::string failure;
		Document canvasDocument, restored;
		REQUIRE(studio::SaveImageGraphCanvas(ui.Graph, basis, ui.Ids, canvasDocument, failure));
		Diagnostic diagnostic;
		REQUIRE(Read(Write(canvasDocument), restored, diagnostic) == Status::Ok);
		CHECK(restored == canvasDocument);
		return restored;
	}

	void FrameAndUnframe(Document document) {
		CanvasInput ui;
		std::string failure;
		REQUIRE(studio::LoadImageGraphCanvas(document, ui.Graph, ui.Ids, failure));
		const Image invertedBefore = EvaluateOutput(document, "inverted");
		const Image kisBefore = EvaluateOutput(document, "kis_output");
		// The loaded Hilbert edge crosses the frame boundary to the outside invert node.
		REQUIRE(ui.Graph.Links().size() == 1);
		ui.Canvas.Select({ui.Ids.ToCanvas.at("hilbert"), ui.Ids.ToCanvas.at("kis")});
		ui.Frame();
		REQUIRE(ui.Focused);
		ui.GroupKey(false);
		REQUIRE(ui.Graph.Groups().size() == 1);
		auto grouped = SaveRoundTrip(ui, document);
		CHECK(grouped.Nodes[0].Id == "hilbert");
		CHECK(grouped.Nodes[1].Id == "invert");
		CHECK_FALSE(grouped.Nodes[0].GroupId.empty());
		CHECK(grouped.Nodes[0].GroupId == grouped.Nodes[2].GroupId);
		CHECK(grouped.Nodes[1].GroupId.empty());
		for (size_t index = 0; index < document.Nodes.size(); ++index)
			CHECK(grouped.Nodes[index].Values == document.Nodes[index].Values);
		CHECK(grouped.Links == document.Links);
		CHECK(grouped.Outputs == document.Outputs);
		CHECK(EvaluateOutput(grouped, "inverted") == invertedBefore);
		CHECK(EvaluateOutput(grouped, "kis_output") == kisBefore);

		CanvasInput reopened;
		REQUIRE(studio::LoadImageGraphCanvas(grouped, reopened.Graph, reopened.Ids, failure));
		REQUIRE(reopened.Graph.Groups().size() == 1);
		reopened.Canvas.Select(reopened.Ids.ToCanvas.at("hilbert"));
		reopened.Frame();
		REQUIRE(reopened.Focused);
		reopened.GroupKey(true);
		CHECK(reopened.Graph.Groups().empty());
		auto ungrouped = SaveRoundTrip(reopened, grouped);
		CHECK(ungrouped.Nodes[0].Id == "hilbert");
		CHECK(ungrouped.Nodes[1].Id == "invert");
		CHECK(ungrouped.Nodes[0].GroupId.empty());
		CHECK(ungrouped.Nodes[1].GroupId.empty());
		CHECK(ungrouped.Nodes[2].GroupId.empty());
		for (size_t index = 0; index < document.Nodes.size(); ++index)
			CHECK(ungrouped.Nodes[index].Values == document.Nodes[index].Values);
		CHECK(ungrouped.Links == document.Links);
		CHECK(ungrouped.Outputs == document.Outputs);
		CHECK(EvaluateOutput(ungrouped, "inverted") == invertedBefore);
		CHECK(EvaluateOutput(ungrouped, "kis_output") == kisBefore);
	}
}

TEST_CASE(
	"Hilbert group shortcuts preserve authored data, links, and output pixels", "[studio][pattern_groups]"
) {
	FrameAndUnframe(PatternGraph(3));
}

TEST_CASE("Kisrhombille formats survive group shortcut and native round trips", "[studio][pattern_groups]") {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		DYNAMIC_SECTION("depth choice " << depth) {
			auto document = PatternGraph(depth);
			FrameAndUnframe(document);
			CHECK(EvaluateOutput(document, "kis_output").Format == KIS_FORMATS[size_t(depth - 2)]);
		}
	}
}
