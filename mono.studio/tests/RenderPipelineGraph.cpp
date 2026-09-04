#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <studio/RenderPipelineGraph.hpp>

TEST_SUITE_ID("studio.renderpipelinegraph")
TEST_DEPENDS("engine.graph.pipelinedocument")

using namespace engine::graph;

TEST_CASE("the default PBR pipeline becomes a typed Blender-style node graph", "[studio][pipeline]") {
	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(DefaultPbrDocument(), canvas, error));

	CHECK(canvas.Nodes().size() == 26);

	// The final two links carry the lit colour and depth into the sky pass. Pin
	// them by name below so this count reads as a checksum rather than a mystery.
	CHECK(canvas.Links().size() == 54);
	CHECK(canvas.Ordered().size() == canvas.Nodes().size());

	bool sawSsao = false;
	for (const nodegraph::Node &node : canvas.Nodes()) {
		if (node.Type == "render.pass.ssao") {
			sawSsao = true;
			CHECK(node.Label == "ssao");
			CHECK(canvas.LinkInto(node.Id, "depth") != nullptr);
			CHECK(canvas.LinkInto(node.Id, "normal") != nullptr);
		}
	}
	CHECK(sawSsao);
	const auto shaderLenses =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.shader-lenses";
		});
	REQUIRE(shaderLenses != canvas.Nodes().end());
	CHECK(canvas.LinkInto(shaderLenses->Id, "colour") != nullptr);
	CHECK(canvas.LinkInto(shaderLenses->Id, "depth") != nullptr);
	const auto mirrorCapture =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.mirror-capture";
		});
	REQUIRE(mirrorCapture != canvas.Nodes().end());
	CHECK(canvas.LinkInto(mirrorCapture->Id, "last-frame") != nullptr);
	CHECK(canvas.LinkInto(mirrorCapture->Id, "world-state") != nullptr);
	CHECK(mirrorCapture->Widgets.at("feedback").Text == "last-frame");
	CHECK(mirrorCapture->Widgets.at("max-recursion").Number == 3.0);
	const auto deferredLighting =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.deferred-lighting";
		});
	REQUIRE(deferredLighting != canvas.Nodes().end());
	CHECK(canvas.LinkInto(deferredLighting->Id, "portal-light") != nullptr);
	const auto sky =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.sky";
		});
	REQUIRE(sky != canvas.Nodes().end());
	CHECK(canvas.LinkInto(sky->Id, "colour") != nullptr);
	CHECK(canvas.LinkInto(sky->Id, "depth") != nullptr);
	const auto output =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.output-image";
		});
	REQUIRE(output != canvas.Nodes().end());
	CHECK(canvas.LinkInto(output->Id, "image") != nullptr);
	CHECK(std::none_of(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
		return node.Type == "render.pass.output";
	}));

	const nodegraph::NodeType *gbuffer = nodegraph::NodeTypes::Find("render.pass.gbuffer");
	REQUIRE(gbuffer != nullptr);
	CHECK(gbuffer->PreviewPort == "albedo");
	REQUIRE(gbuffer->Widgets.size() >= 2);
	CHECK(gbuffer->Widgets[0].Key == "preview.enabled");
	CHECK(gbuffer->Widgets[0].Default.Flag);
	CHECK(gbuffer->Widgets[1].Key == "preview.reverse-spectrum");
	CHECK_FALSE(gbuffer->Widgets[1].Default.Flag);
	REQUIRE_FALSE(gbuffer->Outputs.empty());
	CHECK(gbuffer->Outputs.front().Type == "render.image");
	const nodegraph::DataType *image = nodegraph::DataTypes::Find("render.image");
	REQUIRE(image != nullptr);
	CHECK(image->Label == "IMAGE");
	const nodegraph::NodeType *shadow = nodegraph::NodeTypes::Find("render.pass.shadow");
	REQUIRE(shadow != nullptr);
	CHECK(shadow->PreviewPort == "shadow");
	REQUIRE(shadow->Outputs.size() == 1);
	CHECK(shadow->Outputs.front().Type == "render.image");
	for (const char *kind : {"portal-capture", "portal-tonemap", "portal-overlay", "mirror-overlay"}) {
		const nodegraph::NodeType *pass = nodegraph::NodeTypes::Find(std::string("render.pass.") + kind);
		REQUIRE(pass != nullptr);
		CHECK_FALSE(pass->PreviewPort.empty());
	}
}

TEST_CASE(
	"mirror feedback and its bounded recursion are saved as node policy", "[studio][pipeline][mirror]"
) {
	const PipelineDocument basis = DefaultPbrDocument();
	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	const auto mirror = std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const auto &node) {
		return node.Type == "render.pass.mirror-capture";
	});
	REQUIRE(mirror != canvas.Nodes().end());
	mirror->Widgets.at("feedback").Text = "last-frame";
	mirror->Widgets.at("max-recursion").Number = 2.0;

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(Build(saved, graph, offender) == PipelineDocumentStatus::Ok);
	const Node *captured = nullptr;
	for (uint32_t value = 1; value <= graph.Count(); value++) {
		const Node *candidate = graph.Find(NodeId{value});
		if (candidate != nullptr && candidate->Kind == engine::core::Name("mirror-capture")) {
			captured = candidate;
			break;
		}
	}
	REQUIRE(captured != nullptr);
	REQUIRE(captured->Parameter(engine::core::Name("feedback")) != nullptr);
	CHECK(*captured->Parameter(engine::core::Name("feedback")) == "last-frame");
	CHECK(captured->Integer(engine::core::Name("max-recursion"), 0) == 2);
}

TEST_CASE("preview controls belong to image nodes and round trip with the profile", "[studio][pipeline]") {
	const PipelineDocument basis = DefaultPbrDocument();
	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));

	for (nodegraph::Node &node : canvas.Nodes()) {
		const nodegraph::NodeType *type = nodegraph::NodeTypes::Find(node.Type);
		REQUIRE(type != nullptr);
		if (type->PreviewPort.empty()) {
			CHECK_FALSE(node.Widgets.contains("preview.enabled"));
			CHECK_FALSE(node.Widgets.contains("preview.reverse-spectrum"));
			continue;
		}
		REQUIRE(node.Widgets.contains("preview.enabled"));
		REQUIRE(node.Widgets.contains("preview.reverse-spectrum"));
		if (node.Type == "render.pass.gbuffer") {
			node.Widgets["preview.enabled"].Flag = false;
			node.Widgets["preview.reverse-spectrum"].Flag = true;
		}
	}

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	nodegraph::Graph restored;
	REQUIRE(studio::LoadRenderPipelineGraph(saved, restored, error));
	const auto gbuffer = std::find_if(restored.Nodes().begin(), restored.Nodes().end(), [](const auto &node) {
		return node.Type == "render.pass.gbuffer";
	});
	REQUIRE(gbuffer != restored.Nodes().end());
	CHECK_FALSE(gbuffer->Widgets.at("preview.enabled").Flag);
	CHECK(gbuffer->Widgets.at("preview.reverse-spectrum").Flag);
}

TEST_CASE("a canvas edit round trips to a schedulable world document", "[studio][pipeline]") {
	const PipelineDocument basis = DefaultPbrDocument();
	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));

	for (nodegraph::Node &node : canvas.Nodes()) {
		if (node.Type != "render.pass.ssao") {
			continue;
		}
		nodegraph::Value queue;
		queue.Kind = nodegraph::WidgetKind::Select;
		queue.Text = "compute";
		node.Widgets["queue"] = queue;

		nodegraph::Value async;
		async.Kind = nodegraph::WidgetKind::Select;
		async.Text = "allow";
		node.Widgets["async"] = async;
	}

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));

	RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(Build(saved, graph, offender) == PipelineDocumentStatus::Ok);
	ExecutionSchedule schedule;
	REQUIRE(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);

	bool scheduledAsCompute = false;
	for (const ExecutionWave &wave : schedule.Waves) {
		for (const ScheduledNode &scheduled : wave.Nodes) {
			const Node *node = graph.Find(scheduled.Node);
			if (node != nullptr && node->Name == engine::core::Name("ssao")) {
				scheduledAsCompute = scheduled.Queue == ExecutionQueue::Compute && scheduled.AsyncEligible;
			}
		}
	}
	CHECK(scheduledAsCompute);
}

TEST_CASE(
	"dispatch and output controls are graph data rather than decorative widgets", "[studio][pipeline]"
) {
	studio::RegisterRenderPipelineNodeTypes();
	nodegraph::Graph blitCanvas;
	const nodegraph::NodeId blitId = blitCanvas.Add("render.pass.blit", 0.0f, 0.0f);
	REQUIRE(blitId != nodegraph::NO_NODE);
	REQUIRE(blitCanvas.Find(blitId)->Widgets.contains("format"));
	blitCanvas.Find(blitId)->Widgets["format"].Text = "RGB10A2";
	PipelineDocument blitDocument;
	std::string blitError;
	REQUIRE(studio::SaveRenderPipelineGraph(blitCanvas, {}, blitDocument, blitError));
	const auto blitResource =
		std::find_if(blitDocument.Edits().begin(), blitDocument.Edits().end(), [](const Edit &edit) {
			return edit.Kind == EditKind::AddResource;
		});
	REQUIRE(blitResource != blitDocument.Edits().end());
	CHECK(blitResource->Format == ResourceFormat::RGB10A2);

	nodegraph::Graph dispatchCanvas;
	const nodegraph::NodeId dispatchId = dispatchCanvas.Add("render.pass.dispatch", 0.0f, 0.0f);
	REQUIRE(dispatchId != nodegraph::NO_NODE);
	const nodegraph::Node *dispatch = dispatchCanvas.Find(dispatchId);
	REQUIRE(dispatch != nullptr);
	CHECK(dispatch->Widgets.contains("dispatch.x"));
	CHECK(dispatch->Widgets.contains("dispatch.y"));
	CHECK(dispatch->Widgets.contains("dispatch.z"));
	CHECK(dispatch->Widgets.contains("dispatch.mode"));
	CHECK(dispatch->Widgets.contains("local.x"));
	CHECK(dispatch->Widgets.contains("local.y"));
	CHECK(dispatch->Widgets.contains("local.z"));
	CHECK(dispatch->Widgets.contains("shader"));
	CHECK(dispatch->Widgets.contains("source"));

	nodegraph::Graph rasterCanvas;
	const nodegraph::NodeId rasterId = rasterCanvas.Add("render.pass.raster", 0.0f, 0.0f);
	REQUIRE(rasterId != nodegraph::NO_NODE);
	const nodegraph::Node *raster = rasterCanvas.Find(rasterId);
	REQUIRE(raster != nullptr);
	CHECK(raster->Widgets.contains("shader"));
	CHECK(raster->Widgets.contains("source"));
	CHECK(raster->Widgets.contains("load"));
	rasterCanvas.Find(rasterId)->Widgets["source"].Text = "#version 450\nvoid main() {}";
	rasterCanvas.Find(rasterId)->Widgets["load"].Text = "load";
	PipelineDocument rasterDocument;
	std::string rasterError;
	REQUIRE(studio::SaveRenderPipelineGraph(rasterCanvas, {}, rasterDocument, rasterError));
	RenderGraph rasterGraph;
	engine::core::Name rasterOffender;
	REQUIRE(Build(rasterDocument, rasterGraph, rasterOffender) == PipelineDocumentStatus::Ok);
	const Node *savedRaster = rasterGraph.Find(NodeId{1});
	REQUIRE(savedRaster != nullptr);
	REQUIRE(savedRaster->Parameter(engine::core::Name("source")) != nullptr);
	CHECK(*savedRaster->Parameter(engine::core::Name("source")) == "#version 450\nvoid main() {}");
	REQUIRE(savedRaster->Parameter(engine::core::Name("load")) != nullptr);
	CHECK(*savedRaster->Parameter(engine::core::Name("load")) == "load");
	PipelineDocument exportedRaster;
	engine::core::Name exportOffender;
	REQUIRE(Read(Write(rasterDocument), exportedRaster, exportOffender) == PipelineDocumentStatus::Ok);
	RenderGraph exportedGraph;
	REQUIRE(Build(exportedRaster, exportedGraph, exportOffender) == PipelineDocumentStatus::Ok);
	const Node *exportedNode = exportedGraph.Find(NodeId{1});
	REQUIRE(exportedNode != nullptr);
	REQUIRE(exportedNode->Parameter(engine::core::Name("source")) != nullptr);
	CHECK(*exportedNode->Parameter(engine::core::Name("source")) == "#version 450\nvoid main() {}");

	nodegraph::Graph viewerCanvas;
	const nodegraph::NodeId viewerId = viewerCanvas.Add("render.pass.viewer", 0.0f, 0.0f);
	REQUIRE(viewerId != nodegraph::NO_NODE);
	CHECK(viewerCanvas.Find(viewerId)->Widgets.contains("view"));

	nodegraph::Graph captureCanvas;
	const nodegraph::NodeId captureId = captureCanvas.Add("render.pass.capture", 0.0f, 0.0f);
	REQUIRE(captureId != nodegraph::NO_NODE);
	const nodegraph::Node *capture = captureCanvas.Find(captureId);
	REQUIRE(capture != nullptr);
	CHECK(capture->Widgets.contains("view"));
	CHECK(capture->Widgets.contains("path"));
	CHECK(capture->Widgets.contains("capture.mode"));

	const PipelineDocument basis = DefaultPbrDocument();
	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));

	for (nodegraph::Node &node : canvas.Nodes()) {
		if (node.Type != "render.pass.ssao") {
			continue;
		}
		node.Widgets["resource.occlusion.resolution"].Text = "quarter";
		node.Widgets["resource.occlusion.lifetime"].Text = "external";
	}

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	bool found = false;
	for (const Edit &edit : saved.Edits()) {
		if (edit.Kind == EditKind::AddResource && edit.Name == engine::core::Name("occlusion")) {
			found = true;
			CHECK(edit.Divisor == 4);
			CHECK(edit.External);
		}
	}
	CHECK(found);
}

TEST_CASE("entity filters expose only controls their backend executes", "[studio][pipeline][cull]") {
	studio::RegisterRenderPipelineNodeTypes();
	nodegraph::Graph canvas;
	const nodegraph::NodeId frustumId = canvas.Add("render.pass.cull-frustum", 0.0f, 0.0f);
	const nodegraph::NodeId distanceId = canvas.Add("render.pass.cull-distance", 200.0f, 0.0f);
	const nodegraph::NodeId tagId = canvas.Add("render.pass.filter-tag", 400.0f, 0.0f);
	const nodegraph::NodeId gbufferId = canvas.Add("render.pass.gbuffer", 600.0f, 0.0f);
	REQUIRE(frustumId != nodegraph::NO_NODE);
	REQUIRE(distanceId != nodegraph::NO_NODE);
	REQUIRE(tagId != nodegraph::NO_NODE);
	REQUIRE(gbufferId != nodegraph::NO_NODE);

	CHECK(canvas.Find(frustumId)->Widgets.contains("culling"));
	CHECK(canvas.Find(distanceId)->Widgets.contains("radius"));
	CHECK(canvas.Find(tagId)->Widgets.contains("mask"));
	CHECK_FALSE(canvas.Find(gbufferId)->Widgets.contains("culling"));

	canvas.Find(distanceId)->Widgets["radius"].Text = "128.5";
	canvas.Find(tagId)->Widgets["mask"].Text = "0x21";
	PipelineDocument saved;
	std::string error;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, {}, saved, error));

	RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(Build(saved, graph, offender) == PipelineDocumentStatus::Ok);
	const Node *distance = graph.Find(NodeId{2});
	const Node *tag = graph.Find(NodeId{3});
	REQUIRE(distance != nullptr);
	REQUIRE(tag != nullptr);
	CHECK(distance->Number(engine::core::Name("radius"), 0.0f) == 128.5f);
	CHECK(tag->Integer(engine::core::Name("mask"), 0) == 0x21);
}

TEST_CASE("the IMAGE socket rejects a non-image before save", "[studio][pipeline]") {
	studio::RegisterRenderPipelineNodeTypes();
	nodegraph::Graph canvas;
	const nodegraph::NodeId entities = canvas.Add("render.pass.entities", 0.0f, 0.0f);
	const nodegraph::NodeId tonemap = canvas.Add("render.pass.tonemap", 200.0f, 0.0f);
	REQUIRE(entities != nodegraph::NO_NODE);
	REQUIRE(tonemap != nodegraph::NO_NODE);
	CHECK(canvas.Connect(entities, "entities", tonemap, "colour") == nodegraph::LinkResult::TypeMismatch);
}
