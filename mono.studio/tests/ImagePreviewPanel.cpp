#include "ImagePreviewPanel.hpp"

#include "ImageGraphPreviewResult.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nodegraph/Layout.hpp>

TEST_SUITE_ID("studio.imagegraph.node_preview")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("studio.imagegraph.preview_result")

namespace {
	using namespace engine::imagegraph;
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{8}}, {"height", int64_t{4}}, {"colour", Colour{255, 64, 128, 255}}}},
			{"preview", "pc.graph_preview", {}, {}, {}}
		};
		document.Links = {{"source", "image", "preview", "surface"}};
		document.Outputs = {{"final", "source", "image"}};
		return document;
	}
}
TEST_CASE(
	"embedded image previews keep the authored outputs and survive document history",
	"[studio][imagegraph][node-preview]"
) {
	using namespace engine::imagegraph;
	const auto source = Graph();
	Document preview;
	REQUIRE(studio::detail::MakeImagePreviewDocument(source, "preview", preview));
	CHECK(source.Outputs.front().Id == "final");
	REQUIRE(preview.Outputs.size() == 1);
	CHECK(preview.Outputs.front().NodeId == "source");
	Diagnostic error;
	Document loaded;
	REQUIRE(Read(Write(source), loaded, error) == Status::Ok);
	CHECK(loaded == source);
	studio::ImageGraphHistory history;
	auto edited = source;
	edited.Nodes.pop_back();
	edited.Links.clear();
	history.Record(source, edited);
	REQUIRE(history.Undo(edited));
	CHECK(edited == source);
	REQUIRE(history.Redo(edited));
	CHECK_FALSE(studio::detail::MakeImagePreviewDocument(edited, "preview", preview));
}
TEST_CASE(
	"embedded image previews evaluate an independent image input and tolerate an unwired node",
	"[studio][imagegraph][node-preview]"
) {
	using namespace engine::imagegraph;
	auto source = Graph();
	Document preview;
	REQUIRE(studio::detail::MakeImagePreviewDocument(source, "preview", preview));
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(preview, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.MaximumImageDimension = 128;
	CapturedFeedbackHost feedback;
	studio::ImageGraphPreviewValue result;
	REQUIRE(
		studio::detail::PrepareImageGraphPreviewResult(
			preview, plan, "embedded-preview", request, feedback, 1, 1, result, error
		) == Status::Ok
	);
	const auto *image = std::get_if<Image>(&result);
	REQUIRE(image);
	CHECK(image->Width == 8);
	CHECK(image->Height == 4);
	CHECK(image->Pixels.size() == 128);
	const auto oldHash = image->Hash;
	preview.Nodes.front().Values[0].Data = int64_t{4};
	preview.Nodes.front().Values[2].Data = Colour{0, 255, 0, 255};
	REQUIRE(Compile(preview, plan, error) == Status::Ok);
	REQUIRE(
		studio::detail::PrepareImageGraphPreviewResult(
			preview, plan, "embedded-preview", request, feedback, 2, 1, result, error
		) == Status::Ok
	);
	CHECK(std::get<Image>(result).Width == 4);
	CHECK(std::get<Image>(result).Hash != oldHash);
	source.Links.clear();
	CHECK_FALSE(studio::detail::MakeImagePreviewDocument(source, "preview", preview));
	REQUIRE(Compile(source, plan, error) == Status::Ok);
	studio::ImageGraphPreviewValue mainResult;
	REQUIRE(
		studio::EvaluateImageGraphPreview(source, plan, "final", request, mainResult, error) == Status::Ok
	);
	CHECK(std::get<Image>(mainResult).Width == 8);
}
TEST_CASE(
	"embedded image preview body preserves other canvas body callbacks", "[studio][imagegraph][node-preview]"
) {
	studio::RegisterImageGraphNodeTypes();
	nodegraph::Canvas canvas;
	canvas.Signals.MeasureBody = [](const nodegraph::Node &) { return nodegraph::BodySize{63, 42}; };
	studio::ImageGraphCanvasIds ids;
	studio::detail::ImagePreviewPanel previews;
	previews.Attach(canvas, ids);
	nodegraph::Node preview;
	preview.Type = "pc.graph_preview";
	const auto body = canvas.Signals.MeasureBody(preview);
	CHECK(body.Width == 192);
	CHECK(body.Height == 144);
	preview.Type = "pc.vector2";
	CHECK(canvas.Signals.MeasureBody(preview).Width == 63);
	CHECK(canvas.Signals.MeasureBody(preview).Height == 42);
}
TEST_CASE(
	"embedded preview refresh caches failed capability attempts until its identity changes",
	"[studio][imagegraph][node-preview]"
) {
	using namespace engine::imagegraph;
	struct CountingCamera final : HostNodeProvider {
		size_t Calls = 0;
		bool Capture(const HostNodeInvocation &, HostNodeCapture &, std::string &failure) override {
			++Calls;
			failure = "camera capture unavailable";
			return false;
		}
	} camera;
	Document document;
	document.FormatVersion = 9;
	document.Project.emplace();
	document.Project->SurfaceWidth = 8;
	document.Project->SurfaceHeight = 4;
	document.Project->Shader3D = 1;
	document.Nodes = {
		{"cube", "pc.3_d_mesh_cube", {}, {}, {}},
		{"scene", "pc.3_d_scene", {}, {}, {}},
		{"camera", "pc.3_d_camera", {}, {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{1}}}},
		{"preview", "pc.graph_preview", {}, {}, {}}
	};
	document.Nodes[1].DynamicInputs = {{"cube", ValueType::Mesh, std::nullopt}};
	document.Links = {
		{"cube", "mesh", "scene", "cube"},
		{"scene", "scene", "camera", "scene"},
		{"camera", "rendered", "preview", "surface"}
	};
	studio::detail::ImageGraphHost host;
	host.Composer = &camera;
	engine::render::Renderer renderer;
	studio::detail::ImagePreviewPanel previews;
	EvaluationRequest request;
	request.MaximumImageDimension = 128;
	previews.Refresh(document, request, host, renderer, 1, 1, 0);
	CHECK(previews.Message("preview") == "camera capture unavailable");
	REQUIRE(camera.Calls == 1);
	previews.Refresh(document, request, host, renderer, 1, 1, 0);
	CHECK(camera.Calls == 1);
	previews.Request();
	previews.Refresh(document, request, host, renderer, 1, 1, 0);
	CHECK(camera.Calls == 2);
	previews.Refresh(document, request, host, renderer, 2, 1, 0);
	CHECK(camera.Calls == 3);
	previews.Refresh(document, request, host, renderer, 2, 2, 0);
	CHECK(camera.Calls == 4);
	request.Tick = 1;
	previews.Refresh(document, request, host, renderer, 2, 2, 0);
	CHECK(camera.Calls == 5);
	document.Nodes.pop_back();
	document.Links.pop_back();
	previews.Refresh(document, request, host, renderer, 3, 2, 0);
	CHECK(camera.Calls == 5);
	previews.Close(renderer);
}
TEST_CASE(
	"embedded solid preview reaches the display upload boundary without a GPU",
	"[studio][imagegraph][node-preview]"
) {
	studio::detail::ImagePreviewPanel previews;
	studio::detail::ImageGraphHost host;
	engine::render::Renderer renderer;
	engine::imagegraph::EvaluationRequest request;
	request.MaximumImageDimension = 128;
	const auto document = Graph();
	previews.Refresh(document, request, host, renderer, 1, 1, 0);
	CHECK(previews.Message("preview") == "Preview upload failed");
	previews.Refresh(document, request, host, renderer, 1, 1, 0);
	CHECK(previews.Message("preview") == "Preview upload failed");
	previews.Close(renderer);
	CHECK(previews.Message("preview").empty());
}
