#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <studio/RenderPipelineGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.renderpipelinegraph")
TEST_DEPENDS("engine.graph.pipelinedocument")

using namespace engine::graph;

TEST_CASE("the default PBR pipeline becomes a typed Blender-style node graph", "[studio][pipeline]") {
	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(DefaultPbrDocument(), canvas, error));

	CHECK(canvas.Nodes().size() == 32);

	// Pin the default pipeline's node and link counts so stage changes update this checksum.
	CHECK(canvas.Links().size() == 64);
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
	const auto depthPeel =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.depth-peel";
		});
	REQUIRE(depthPeel != canvas.Nodes().end());
	CHECK_FALSE(depthPeel->Widgets.at("enabled").Flag);
	CHECK(canvas.LinkInto(depthPeel->Id, "first-depth") != nullptr);
	const auto meshResidency =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.mesh-residency";
		});
	REQUIRE(meshResidency != canvas.Nodes().end());
	const auto deltaUpload =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.delta-upload";
		});
	REQUIRE(deltaUpload != canvas.Nodes().end());
	CHECK(canvas.LinkInto(deltaUpload->Id, "meshes") != nullptr);
	const auto shaderLenses =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.shader-lenses";
		});
	REQUIRE(shaderLenses != canvas.Nodes().end());
	CHECK(canvas.LinkInto(shaderLenses->Id, "colour") != nullptr);
	CHECK(canvas.LinkInto(shaderLenses->Id, "depth") != nullptr);
	const auto bloom =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.bloom";
		});
	REQUIRE(bloom != canvas.Nodes().end());
	CHECK(canvas.LinkInto(bloom->Id, "source") != nullptr);
	const auto surfaceCapture =
		std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const nodegraph::Node &node) {
			return node.Type == "render.pass.surface-capture";
		});
	REQUIRE(surfaceCapture != canvas.Nodes().end());
	CHECK(canvas.LinkInto(surfaceCapture->Id, "world-state") != nullptr);
	CHECK(canvas.LinkInto(surfaceCapture->Id, "shadow") != nullptr);
	CHECK(canvas.LinkInto(surfaceCapture->Id, "entities") != nullptr);
	CHECK(canvas.LinkInto(surfaceCapture->Id, "instances") != nullptr);
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

TEST_CASE("canvas save keeps authoring metadata without canvas controls", "[studio][pipeline]") {
	PipelineDocument basis = DefaultPbrDocument();
	PipelineDocument expected;
	for (const Edit &edit : std::array{
			 Edit{
				 .Kind = EditKind::Group,
				 .Name = engine::core::Name("lighting"),
				 .Target = engine::core::Name("gbuffer")
			 },
			 Edit{.Kind = EditKind::Comment, .Name = engine::core::Name("gbuffer"), .Value = "world pass"},
			 Edit{.Kind = EditKind::Mute, .Name = engine::core::Name("ssao"), .Enabled = true},
			 Edit{.Kind = EditKind::Preview, .Target = engine::core::Name("albedo")},
		 }) {
		basis.Record(edit);
		expected.Record(edit);
	}

	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	PipelineDocument actual;
	for (const Edit &edit : saved.Edits())
		if (edit.Kind == EditKind::Group || edit.Kind == EditKind::Comment || edit.Kind == EditKind::Mute ||
			edit.Kind == EditKind::Preview)
			actual.Record(edit);
	CHECK(Write(actual) == Write(expected));
	RenderGraph rebuilt;
	engine::core::Name offender;
	CHECK(Build(saved, rebuilt, offender) == PipelineDocumentStatus::Ok);
}

TEST_CASE("canvas document round trip retains resource and unknown authored data", "[studio][pipeline]") {
	PipelineDocument basis;
	basis.Record(
		{.Kind = EditKind::AddResource,
		 .Name = engine::core::Name("source"),
		 .Resource = ResourceKind::Colour,
		 .Format = ResourceFormat::RGBA16F,
		 .Divisor = 2,
		 .External = true}
	);
	basis.Record(
		{.Kind = EditKind::AddResource,
		 .Name = engine::core::Name("display"),
		 .Resource = ResourceKind::Colour,
		 .Format = ResourceFormat::RGBA16F,
		 .Divisor = 3,
		 .Access = ResourceAccess::Write,
		 .Samples = 4,
		 .Depth = 3,
		 .Layers = 6,
		 .FirstMip = 2,
		 .MipCount = 5,
		 .ColourSpace = ResourceColourSpace::SRGB,
		 .AlphaSpace = ResourceAlphaSpace::Straight,
		 .BufferStride = 68,
		 .Lifetime = ResourceLifetime::External,
		 .Owner = engine::core::Name("display-owner"),
		 .HistoryGeneration = 7}
	);
	basis.Record(
		{.Kind = EditKind::AddNode,
		 .Name = engine::core::Name("tone"),
		 .NodeKind = engine::core::Name("tonemap"),
		 .Scope = NodeScope::View}
	);
	basis.Record(
		{.Kind = EditKind::Reads, .Target = engine::core::Name("source"), .Key = engine::core::Name("colour")}
	);
	basis.Record(
		{.Kind = EditKind::Writes,
		 .Target = engine::core::Name("display"),
		 .Key = engine::core::Name("display")}
	);
	basis.Record(
		{.Kind = EditKind::Set,
		 .Key = engine::core::Name("unknown-authored-text"),
		 .Value = "leave this text alone"}
	);
	basis.Record({.Kind = EditKind::Enable, .Name = engine::core::Name("tone"), .Enabled = true});
	basis.Record({.Kind = EditKind::Move, .Name = engine::core::Name("tone"), .X = 140.0f, .Y = 80.0f});

	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	REQUIRE(canvas.Nodes().size() == 1);
	CHECK(
		canvas.Nodes().front().Widgets.at("__render.parameter.unknown-authored-text").Text ==
		"leave this text alone"
	);

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	const auto source = std::find_if(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::AddResource && edit.Name == engine::core::Name("source");
	});
	REQUIRE(source != saved.Edits().end());
	CHECK(source->External);
	CHECK(source->Divisor == 2);
	CHECK(source->Format == ResourceFormat::RGBA16F);
	const auto display = std::find_if(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::AddResource && edit.Name == engine::core::Name("display");
	});
	REQUIRE(display != saved.Edits().end());
	CHECK(display->Divisor == 3);
	CHECK_FALSE(display->External);
	CHECK(display->Access == ResourceAccess::Write);
	CHECK(display->Samples == 4);
	CHECK(display->Depth == 3);
	CHECK(display->Layers == 6);
	CHECK(display->FirstMip == 2);
	CHECK(display->MipCount == 5);
	CHECK(display->ColourSpace == ResourceColourSpace::SRGB);
	CHECK(display->AlphaSpace == ResourceAlphaSpace::Straight);
	CHECK(display->BufferStride == 68);
	CHECK(display->Lifetime == ResourceLifetime::External);
	CHECK(display->Owner == engine::core::Name("display-owner"));
	CHECK(display->HistoryGeneration == 7);
	const auto tone = std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const auto &node) {
		return node.Label == "tone";
	});
	REQUIRE(tone != canvas.Nodes().end());
	CHECK(tone->Widgets.at("resource.display.lifetime").Text == "external");
	const auto unknown = std::find_if(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::Set && edit.Key == engine::core::Name("unknown-authored-text");
	});
	REQUIRE(unknown != saved.Edits().end());
	CHECK(unknown->Value == "leave this text alone");

	nodegraph::Graph restored;
	REQUIRE(studio::LoadRenderPipelineGraph(saved, restored, error));
	PipelineDocument canonical;
	REQUIRE(studio::SaveRenderPipelineGraph(restored, saved, canonical, error));
	CHECK(Write(canonical) == Write(saved));
}

TEST_CASE("authored binding order survives the nodegraph adapter", "[studio][pipeline]") {
	const PipelineDocument defaults = DefaultPbrDocument();
	std::vector<Edit> edits(defaults.Edits().begin(), defaults.Edits().end());
	const NodeKindSpec *ssao = NodeCatalogue::Find(engine::core::Name("ssao"));
	REQUIRE(ssao != nullptr);
	REQUIRE(ssao->Inputs.size() >= 2);
	const NodeKindSpec *gbuffer = NodeCatalogue::Find(engine::core::Name("gbuffer"));
	REQUIRE(gbuffer != nullptr);
	REQUIRE(gbuffer->Outputs.size() >= 2);

	std::vector<size_t> readSlots;
	std::vector<size_t> writeSlots;
	for (size_t index = 0; index < edits.size(); ++index) {
		if (edits[index].Kind == EditKind::AddNode && edits[index].Name == engine::core::Name("ssao")) {
			for (size_t next = index + 1; next < edits.size(); ++next) {
				if (edits[next].Kind == EditKind::AddNode || edits[next].Kind == EditKind::AddResource) {
					break;
				}
				if (edits[next].Kind == EditKind::Reads) {
					readSlots.push_back(next);
				}
			}
		} else if (edits[index].Kind == EditKind::AddNode &&
				   edits[index].Name == engine::core::Name("gbuffer")) {
			for (size_t next = index + 1; next < edits.size(); ++next) {
				if (edits[next].Kind == EditKind::AddNode || edits[next].Kind == EditKind::AddResource) {
					break;
				}
				if (edits[next].Kind == EditKind::Writes) {
					writeSlots.push_back(next);
				}
			}
		}
	}
	REQUIRE(readSlots.size() >= 2);
	REQUIRE(writeSlots.size() >= 2);
	std::vector<Edit> reads;
	for (const size_t slot : readSlots) {
		reads.push_back(edits[slot]);
	}
	std::reverse(reads.begin(), reads.end());
	for (size_t index = 0; index < readSlots.size(); ++index) {
		edits[readSlots[index]] = reads[index];
	}
	std::vector<Edit> writes;
	for (const size_t slot : writeSlots) {
		writes.push_back(edits[slot]);
	}
	std::reverse(writes.begin(), writes.end());
	for (size_t index = 0; index < writeSlots.size(); ++index) {
		edits[writeSlots[index]] = writes[index];
	}

	PipelineDocument basis;
	for (const Edit &edit : edits) {
		basis.Record(edit);
	}

	std::vector<engine::core::Name> expected;
	for (const size_t slot : readSlots) {
		expected.push_back(edits[slot].Key);
	}
	std::vector<engine::core::Name> expectedWrites;
	for (const size_t slot : writeSlots) {
		expectedWrites.push_back(edits[slot].Key);
	}

	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));

	std::vector<engine::core::Name> actual;
	std::vector<engine::core::Name> actualWrites;
	bool inSsao = false;
	bool inGbuffer = false;
	for (const Edit &edit : saved.Edits()) {
		if (edit.Kind == EditKind::AddNode) {
			inSsao = edit.Name == engine::core::Name("ssao");
			inGbuffer = edit.Name == engine::core::Name("gbuffer");
		} else if (inSsao && edit.Kind == EditKind::Reads) {
			actual.push_back(edit.Key);
		} else if (inGbuffer && edit.Kind == EditKind::Writes) {
			actualWrites.push_back(edit.Key);
		}
	}
	CHECK(actual == expected);
	CHECK(actualWrites == expectedWrites);
}

TEST_CASE("group, comment, mute, and selected preview metadata follow canvas edits", "[studio][pipeline]") {
	PipelineDocument basis = DefaultPbrDocument();
	basis.Record(
		{.Kind = EditKind::Group,
		 .Name = engine::core::Name("lighting"),
		 .Target = engine::core::Name("gbuffer")}
	);
	basis.Record({.Kind = EditKind::Comment, .Name = engine::core::Name("gbuffer"), .Value = "world pass"});
	basis.Record({.Kind = EditKind::Mute, .Name = engine::core::Name("ssao"), .Enabled = true});
	basis.Record({.Kind = EditKind::Preview, .Target = engine::core::Name("albedo")});

	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	REQUIRE(canvas.Groups().size() == 1);
	const nodegraph::Group &lighting = canvas.Groups().front();
	CHECK(lighting.Title == "lighting");
	REQUIRE(lighting.Members.size() == 1);
	const nodegraph::Node *gbuffer = canvas.Find(lighting.Members.front());
	REQUIRE(gbuffer != nullptr);
	CHECK(gbuffer->Label == "gbuffer");
	CHECK(gbuffer->Widgets.at("__render.authoring.comment").Text == "world pass");
	const auto ssaoNode = std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const auto &node) {
		return node.Label == "ssao";
	});
	REQUIRE(ssaoNode != canvas.Nodes().end());
	CHECK(ssaoNode->Widgets.at("__render.authoring.muted").Flag);

	const auto gbufferNode = std::find_if(canvas.Nodes().begin(), canvas.Nodes().end(), [](const auto &node) {
		return node.Label == "gbuffer";
	});
	REQUIRE(gbufferNode != canvas.Nodes().end());
	CHECK(gbufferNode->Widgets.at("__render.authoring.preview").Flag);

	nodegraph::Group *editedLighting = canvas.FindGroup(lighting.Id);
	REQUIRE(editedLighting != nullptr);
	editedLighting->Title = "lighting-edited";
	nodegraph::Node *editedGbuffer = canvas.Find(gbufferNode->Id);
	REQUIRE(editedGbuffer != nullptr);
	editedGbuffer->Label = "gbuffer-edited";
	editedGbuffer->Widgets["__render.authoring.comment"].Text = "edited pass";

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	std::vector<Edit> metadata;
	for (const Edit &edit : saved.Edits()) {
		if (edit.Kind == EditKind::Group || edit.Kind == EditKind::Comment || edit.Kind == EditKind::Mute ||
			edit.Kind == EditKind::Preview) {
			metadata.push_back(edit);
		}
	}
	REQUIRE(metadata.size() == 4);
	CHECK(metadata[0].Kind == EditKind::Group);
	CHECK(metadata[0].Name == engine::core::Name("lighting-edited"));
	CHECK(metadata[0].Target == engine::core::Name("gbuffer-edited"));
	CHECK(metadata[1].Kind == EditKind::Comment);
	CHECK(metadata[1].Name == engine::core::Name("gbuffer-edited"));
	CHECK(metadata[1].Value == "edited pass");
	CHECK(metadata[2].Kind == EditKind::Mute);
	CHECK(metadata[2].Name == engine::core::Name("ssao"));
	CHECK(metadata[2].Enabled);
	CHECK(metadata[3].Kind == EditKind::Preview);
	CHECK(metadata[3].Target == engine::core::Name("albedo"));
}

TEST_CASE("partial canvas records round trip without graph validation", "[studio][pipeline]") {
	PipelineDocument basis;
	basis.Record({.Kind = EditKind::Set, .Key = engine::core::Name("orphan"), .Value = "keep me"});
	basis.Record(
		{.Kind = EditKind::AddNode,
		 .Name = engine::core::Name("tone"),
		 .NodeKind = engine::core::Name("tonemap"),
		 .Scope = NodeScope::View}
	);
	basis.Record({.Kind = EditKind::Reads, .Key = engine::core::Name("colour")});
	basis.Record(
		{.Kind = EditKind::Writes,
		 .Target = engine::core::Name("display"),
		 .Key = engine::core::Name("display")}
	);

	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));

	const auto orphan = std::find_if(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::Set && edit.Key == engine::core::Name("orphan");
	});
	REQUIRE(orphan != saved.Edits().end());
	CHECK(orphan->Value == "keep me");
	const auto unbound = std::find_if(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::Reads && edit.Key == engine::core::Name("colour");
	});
	REQUIRE(unbound != saved.Edits().end());
	CHECK_FALSE(unbound->Target.IsValid());

	RenderGraph rebuilt;
	engine::core::Name offender;
	CHECK(Build(saved, rebuilt, offender) == PipelineDocumentStatus::UnknownName);
	CHECK(offender == engine::core::Name("display"));
}

TEST_CASE("signed compositor numbers survive editor save and reload", "[studio][pipeline][compositor]") {
	studio::RegisterRenderPipelineNodeTypes();
	const PipelineDocument basis = engine::graph::CompositorDemoDocument();
	nodegraph::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));

	const auto byLabel = [](nodegraph::Graph &graph, std::string_view label) -> nodegraph::Node * {
		const auto found = std::find_if(graph.Nodes().begin(), graph.Nodes().end(), [&](const auto &node) {
			return node.Label == label;
		});
		return found == graph.Nodes().end() ? nullptr : &*found;
	};
	nodegraph::Node *exposure = byLabel(canvas, "grade-exposure");
	nodegraph::Node *hsv = byLabel(canvas, "grade-hsv");
	nodegraph::Node *transform = byLabel(canvas, "frame-transform");
	nodegraph::Node *blur = byLabel(canvas, "blur-x");
	REQUIRE(exposure != nullptr);
	REQUIRE(hsv != nullptr);
	REQUIRE(transform != nullptr);
	REQUIRE(blur != nullptr);
	exposure->Widgets["exposure"].Number = -2.5;
	exposure->Widgets["contrast"].Number = 1.125;
	exposure->Widgets["gamma"].Number = 100.0;
	hsv->Widgets["hue"].Number = -45.25;
	transform->Widgets["translate-x"].Number = -0.125;
	transform->Widgets["rotation"].Number = -30.5;
	blur->Widgets["radius"].Number = 100.0;

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	PipelineDocument reloadedDocument;
	engine::core::Name offender;
	REQUIRE(Read(Write(saved), reloadedDocument, offender) == PipelineDocumentStatus::Ok);

	nodegraph::Graph reloaded;
	REQUIRE(studio::LoadRenderPipelineGraph(reloadedDocument, reloaded, error));
	exposure = byLabel(reloaded, "grade-exposure");
	hsv = byLabel(reloaded, "grade-hsv");
	transform = byLabel(reloaded, "frame-transform");
	blur = byLabel(reloaded, "blur-x");
	REQUIRE(exposure != nullptr);
	REQUIRE(hsv != nullptr);
	REQUIRE(transform != nullptr);
	REQUIRE(blur != nullptr);
	CHECK(exposure->Widgets.at("exposure").Number == -2.5);
	CHECK(exposure->Widgets.at("contrast").Number == 1.125);
	CHECK(exposure->Widgets.at("gamma").Number == 8.0);
	CHECK(hsv->Widgets.at("hue").Number == -45.25);
	CHECK(transform->Widgets.at("translate-x").Number == -0.125);
	CHECK(transform->Widgets.at("rotation").Number == -30.5);
	CHECK(blur->Widgets.at("radius").Number == 32.0);
}

TEST_CASE(
	"an authored mirror capture keeps its feedback policy through a canvas round trip",
	"[studio][pipeline][mirror]"
) {
	PipelineDocument basis = DefaultPbrDocument();
	basis.Record({.Kind = EditKind::Enable, .Name = engine::core::Name("surface-capture"), .Enabled = false});
	basis.Record(
		{.Kind = EditKind::AddNode,
		 .Name = engine::core::Name("mirror-capture.policy"),
		 .NodeKind = engine::core::Name("mirror-capture"),
		 .Scope = NodeScope::View}
	);
	for (const auto &[resource, port] : std::array<std::pair<const char *, const char *>, 5>{{
			 {"last-frame", "last-frame"},
			 {"world-entities", "world-state"},
			 {"shadow", "shadow"},
			 {"ordered-entities", "entities"},
			 {"view-instances", "instances"},
		 }}) {
		basis.Record(
			{.Kind = EditKind::Reads, .Target = engine::core::Name(resource), .Key = engine::core::Name(port)}
		);
	}
	basis.Record(
		{.Kind = EditKind::Writes,
		 .Target = engine::core::Name("mirror-views"),
		 .Key = engine::core::Name("surface")}
	);

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
