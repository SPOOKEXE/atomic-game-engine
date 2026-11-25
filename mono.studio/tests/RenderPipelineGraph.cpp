#include <engine/core/Paths.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/World.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <studio/RenderPipelineGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.renderpipelinegraph")
TEST_DEPENDS("engine.graph.pipelinedocument")

using namespace engine::graph;
namespace ang = engine::ang;

namespace {
	const ang::NodeInstance *ByDef(const ang::Graph &graph, std::string_view def) {
		for (const ang::NodeInstance &node : graph.Nodes())
			if (node.Def == def) return &node;
		return nullptr;
	}

	const ang::NodeInstance *ByLabel(const ang::Graph &graph, std::string_view label) {
		for (const ang::NodeInstance &node : graph.Nodes())
			if (studio::RenderPipelineLabel(node) == label) return &node;
		return nullptr;
	}

	const ang::Link *LinkInto(const ang::Graph &graph, const std::string &node, std::string_view port) {
		for (const ang::Link &link : graph.Links())
			if (link.ToNode == node && link.ToPort == port) return &link;
		return nullptr;
	}

	void Set(ang::Graph &graph, const std::string &node, std::string key, ang::Value value) {
		REQUIRE(graph.Apply(ang::edit::SetParam{node, std::move(key), std::move(value)}).Ok);
	}

	std::string Add(ang::Graph &graph, std::string def, engine::core::Vector2 position) {
		const std::string id = graph.NewNodeId(def);
		REQUIRE(graph.Apply(ang::edit::AddNode{{.Id = id, .Def = std::move(def), .Position = position}}).Ok);
		return id;
	}

	std::string TextOf(const ang::NodeInstance &node, std::string_view key) {
		const ang::Value value = studio::RenderPipelineParam(node, key);
		return value.Get<std::string>() != nullptr ? *value.Get<std::string>() : std::string();
	}

	bool FlagOf(const ang::NodeInstance &node, std::string_view key) {
		const ang::Value value = studio::RenderPipelineParam(node, key);
		return value.Get<bool>() != nullptr && *value.Get<bool>();
	}

	double NumberOf(const ang::NodeInstance &node, std::string_view key) {
		return studio::RenderPipelineParam(node, key).AsNumber();
	}

	bool Links(
		const ang::Graph &graph,
		const std::string &from,
		const char *output,
		const std::string &to,
		const char *input
	) {
		return ang::CheckNewLink(graph, studio::RenderPipelineTypes(), {from, output, to, input}).Ok;
	}
}

TEST_CASE("the default PBR pipeline becomes a typed Blender-style node graph", "[studio][pipeline]") {
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(DefaultPbrDocument(), canvas, error));

	CHECK(canvas.Nodes().size() == 39);

	// Pin the default pipeline's node and link counts so stage changes update this checksum.
	CHECK(canvas.Links().size() == 82);
	ang::Plan plan;
	CHECK(ang::Compile(canvas, studio::RenderPipelineTypes(), {}, plan).Ok);
	CHECK(plan.Nodes.size() == canvas.Nodes().size());

	const ang::NodeInstance *ssao = ByDef(canvas, "render.pass.ssao");
	REQUIRE(ssao != nullptr);
	CHECK(ssao->Id == "ssao");
	CHECK(studio::RenderPipelineLabel(*ssao) == "ssao");
	CHECK(LinkInto(canvas, ssao->Id, "depth") != nullptr);
	CHECK(LinkInto(canvas, ssao->Id, "normal") != nullptr);
	const ang::NodeInstance *depthPeel = ByDef(canvas, "render.pass.depth-peel");
	REQUIRE(depthPeel != nullptr);
	CHECK_FALSE(FlagOf(*depthPeel, "enabled"));
	CHECK(LinkInto(canvas, depthPeel->Id, "first-depth") != nullptr);
	REQUIRE(ByDef(canvas, "render.pass.mesh-residency") != nullptr);
	for (const auto &[def, ports] : std::array<std::pair<const char *, std::vector<const char *>>, 7>{{
			 {"render.pass.delta-upload", {"meshes"}},
			 {"render.pass.shader-lenses", {"colour", "depth"}},
			 {"render.pass.bloom", {"source"}},
			 {"render.pass.surface-capture", {"world-state", "shadow", "entities", "instances"}},
			 {"render.pass.deferred-lighting", {"portal-light"}},
			 {"render.pass.sky", {"colour", "depth"}},
			 {"render.pass.output-image", {"image"}},
		 }}) {
		const ang::NodeInstance *node = ByDef(canvas, def);
		INFO(def);
		REQUIRE(node != nullptr);
		for (const char *port : ports)
			CHECK(LinkInto(canvas, node->Id, port) != nullptr);
	}
	CHECK(ByDef(canvas, "render.pass.output") == nullptr);

	const auto &types = studio::RenderPipelineTypes();
	const auto gbuffer = types.FindNode("render.pass.gbuffer");
	REQUIRE(gbuffer != nullptr);
	CHECK(studio::RenderPipelinePreviewPort("render.pass.gbuffer") == "albedo");
	REQUIRE(gbuffer->Params.size() >= 2);
	CHECK(gbuffer->Params[0].Name == "preview.enabled");
	CHECK(gbuffer->Params[0].Default == ang::Value(true));
	CHECK(gbuffer->Params[1].Name == "preview.reverse-spectrum");
	CHECK(gbuffer->Params[1].Default == ang::Value(false));
	REQUIRE_FALSE(gbuffer->Outputs.empty());
	CHECK(gbuffer->Outputs.front().Type == "render.image");
	const ang::DataType *image = types.FindType("render.image");
	REQUIRE(image != nullptr);
	CHECK(image->Label == "IMAGE");
	const auto shadow = types.FindNode("render.pass.shadow");
	REQUIRE(shadow != nullptr);
	CHECK(studio::RenderPipelinePreviewPort("render.pass.shadow") == "shadow");
	REQUIRE(shadow->Outputs.size() == 1);
	CHECK(shadow->Outputs.front().Type == "render.image");
	for (const char *kind : {"portal-capture", "portal-tonemap", "portal-overlay", "mirror-overlay"}) {
		CHECK_FALSE(studio::RenderPipelinePreviewPort(std::string("render.pass.") + kind).empty());
	}
}

TEST_CASE(
	"weather pins and resource layouts round trip to an executable pipeline", "[studio][pipeline][weather]"
) {
	const PipelineDocument basis = DefaultPbrDocument();
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	const std::array weatherKinds{
		"weather-residency",
		"weather-field",
		"weather-summary",
		"weather-shadow-map",
		"weather-history",
		"weather-integrate",
		"weather-reconstruct"
	};
	for (const char *kind : weatherKinds) {
		const ang::NodeInstance *found = ByDef(canvas, std::string("render.pass.") + kind);
		REQUIRE(found != nullptr);
		const auto type = studio::RenderPipelineTypes().FindNode(found->Def);
		const auto *spec = NodeCatalogue::Find(engine::core::Name(kind));
		REQUIRE(type != nullptr);
		REQUIRE(spec != nullptr);
		CHECK(type->Inputs.size() == spec->Inputs.size());
		CHECK(type->Outputs.size() == spec->Outputs.size());
		for (const auto &input : spec->Inputs) {
			const auto pin =
				std::find_if(type->Inputs.begin(), type->Inputs.end(), [&](const auto &candidate) {
					return candidate.Name == input.Name.Text();
				});
			REQUIRE(pin != type->Inputs.end());
			CHECK(pin->Type == (input.Kind == ResourceKind::Buffer ? "render.buffer" : "render.image"));
			CHECK(LinkInto(canvas, found->Id, pin->Name) != nullptr);
		}
		for (const auto &output : spec->Outputs) {
			const auto pin =
				std::find_if(type->Outputs.begin(), type->Outputs.end(), [&](const auto &candidate) {
					return candidate.Name == output.Name.Text();
				});
			REQUIRE(pin != type->Outputs.end());
			CHECK(pin->Type == (output.Kind == ResourceKind::Buffer ? "render.buffer" : "render.image"));
		}
		if (std::string_view(kind) == "weather-field") {
			Set(canvas, found->Id, "queue", ang::Value("compute"));
			Set(canvas, found->Id, "async", ang::Value("allow"));
			Set(canvas, found->Id, "profile", ang::Value(false));
		}
	}
	const ang::NodeInstance *fog = ByDef(canvas, "render.pass.fog");
	REQUIRE(fog != nullptr);
	const ang::Link *fogInput = LinkInto(canvas, fog->Id, "colour");
	REQUIRE(fogInput != nullptr);
	REQUIRE(canvas.FindNode(fogInput->FromNode) != nullptr);
	CHECK(canvas.FindNode(fogInput->FromNode)->Def == "render.pass.weather-reconstruct");

	PipelineDocument saved;
	const bool savedOk = studio::SaveRenderPipelineGraph(canvas, basis, saved, error);
	INFO(error);
	REQUIRE(savedOk);
	PipelineDocument parsed;
	engine::core::Name offender;
	REQUIRE(Read(Write(saved), parsed, offender) == PipelineDocumentStatus::Ok);
	PipelineDocument expectedResources, actualResources;
	for (const auto &edit : basis.Edits())
		if (edit.Kind == EditKind::AddResource && edit.Name.Text().starts_with("weather-"))
			expectedResources.Record(edit);
	for (const auto &edit : parsed.Edits())
		if (edit.Kind == EditKind::AddResource && edit.Name.Text().starts_with("weather-"))
			actualResources.Record(edit);
	// This includes field strides, page capacities, fixed shadow extents,
	// half-resolution images and external/history lifetimes.
	CHECK(Write(actualResources) == Write(expectedResources));
	RenderGraph original, rebuilt;
	REQUIRE(Build(basis, original, offender) == PipelineDocumentStatus::Ok);
	REQUIRE(Build(parsed, rebuilt, offender) == PipelineDocumentStatus::Ok);
	const auto nodeOf = [](const RenderGraph &graph, const char *kind) -> const Node * {
		for (uint32_t value = 1; value <= graph.Count(); ++value) {
			const auto *node = graph.Find(NodeId{value});
			if (node && node->Kind == engine::core::Name(kind)) return node;
		}
		return nullptr;
	};
	for (const char *kind : weatherKinds) {
		const auto *before = nodeOf(original, kind);
		const auto *after = nodeOf(rebuilt, kind);
		REQUIRE(before != nullptr);
		REQUIRE(after != nullptr);
		CHECK(after->Reads == before->Reads);
		CHECK(after->ReadPorts == before->ReadPorts);
		CHECK(after->Writes == before->Writes);
		CHECK(after->WritePorts == before->WritePorts);
		CHECK(after->Scope == before->Scope);
	}
	const auto *field = nodeOf(rebuilt, "weather-field");
	REQUIRE(field != nullptr);
	REQUIRE(field->Parameter(engine::core::Name("profile")) != nullptr);
	CHECK(*field->Parameter(engine::core::Name("profile")) == "false");
	ExecutionSchedule schedule;
	REQUIRE(CompileSchedule(rebuilt, schedule, offender) == ScheduleStatus::Ok);
	bool computeField = false;
	for (const auto &wave : schedule.Waves)
		for (const auto &scheduled : wave.Nodes)
			if (rebuilt.Find(scheduled.Node) == field)
				computeField = scheduled.Queue == ExecutionQueue::Compute && scheduled.AsyncEligible;
	CHECK(computeField);
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

	ang::Graph canvas;
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

	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	REQUIRE(canvas.Nodes().size() == 1);
	CHECK(
		TextOf(canvas.Nodes().front(), "__render.parameter.unknown-authored-text") == "leave this text alone"
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
	const ang::NodeInstance *tone = ByLabel(canvas, "tone");
	REQUIRE(tone != nullptr);
	CHECK(TextOf(*tone, "resource.display.lifetime") == "external");
	const auto unknown = std::find_if(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::Set && edit.Key == engine::core::Name("unknown-authored-text");
	});
	REQUIRE(unknown != saved.Edits().end());
	CHECK(unknown->Value == "leave this text alone");

	ang::Graph restored;
	REQUIRE(studio::LoadRenderPipelineGraph(saved, restored, error));
	PipelineDocument canonical;
	REQUIRE(studio::SaveRenderPipelineGraph(restored, saved, canonical, error));
	CHECK(Write(canonical) == Write(saved));
}

TEST_CASE("authored binding order survives the canvas adapter", "[studio][pipeline]") {
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

	ang::Graph canvas;
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

	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	REQUIRE(canvas.Groups().size() == 1);
	const ang::Group lighting = canvas.Groups().front();
	CHECK(lighting.Label == "lighting");
	std::vector<std::string> members;
	for (const ang::NodeInstance &node : canvas.Nodes())
		if (node.Group == lighting.Id) members.push_back(node.Id);
	REQUIRE(members.size() == 1);
	const ang::NodeInstance *gbuffer = canvas.FindNode(members.front());
	REQUIRE(gbuffer != nullptr);
	CHECK(studio::RenderPipelineLabel(*gbuffer) == "gbuffer");
	CHECK(TextOf(*gbuffer, "__render.authoring.comment") == "world pass");
	const ang::NodeInstance *ssaoNode = ByLabel(canvas, "ssao");
	REQUIRE(ssaoNode != nullptr);
	CHECK(FlagOf(*ssaoNode, "__render.authoring.muted"));
	CHECK(FlagOf(*gbuffer, "__render.authoring.preview"));

	const std::string gbufferId = gbuffer->Id;
	REQUIRE(canvas.Apply(ang::edit::SetGroupLabel{lighting.Id, "lighting-edited"}).Ok);
	Set(canvas, gbufferId, studio::RENDER_PIPELINE_LABEL, ang::Value("gbuffer-edited"));
	Set(canvas, gbufferId, "__render.authoring.comment", ang::Value("edited pass"));

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

	ang::Graph canvas;
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
	const std::filesystem::path path = engine::core::Paths::Base().parent_path() / "assets" / "examples" /
									   "pipelines" / "CompositorDemo.pipeline";
	std::ifstream input(path, std::ios::binary);
	REQUIRE(input);
	const std::string text(std::istreambuf_iterator<char>(input), {});
	PipelineDocument basis;
	engine::core::Name offender;
	REQUIRE(Read(text, basis, offender) == PipelineDocumentStatus::Ok);
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));

	const ang::NodeInstance *exposure = ByLabel(canvas, "grade-exposure");
	const ang::NodeInstance *hsv = ByLabel(canvas, "grade-hsv");
	const ang::NodeInstance *transform = ByLabel(canvas, "frame-transform");
	const ang::NodeInstance *blur = ByLabel(canvas, "blur-x");
	REQUIRE(exposure != nullptr);
	REQUIRE(hsv != nullptr);
	REQUIRE(transform != nullptr);
	REQUIRE(blur != nullptr);
	Set(canvas, exposure->Id, "exposure", ang::Value(-2.5));
	Set(canvas, exposure->Id, "contrast", ang::Value(1.125));
	Set(canvas, exposure->Id, "gamma", ang::Value(100.0));
	Set(canvas, hsv->Id, "hue", ang::Value(-45.25));
	Set(canvas, transform->Id, "translate-x", ang::Value(-0.125));
	Set(canvas, transform->Id, "rotation", ang::Value(-30.5));
	Set(canvas, blur->Id, "radius", ang::Value(100.0));

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	PipelineDocument reloadedDocument;
	REQUIRE(Read(Write(saved), reloadedDocument, offender) == PipelineDocumentStatus::Ok);

	ang::Graph reloaded;
	REQUIRE(studio::LoadRenderPipelineGraph(reloadedDocument, reloaded, error));
	exposure = ByLabel(reloaded, "grade-exposure");
	hsv = ByLabel(reloaded, "grade-hsv");
	transform = ByLabel(reloaded, "frame-transform");
	blur = ByLabel(reloaded, "blur-x");
	REQUIRE(exposure != nullptr);
	REQUIRE(hsv != nullptr);
	REQUIRE(transform != nullptr);
	REQUIRE(blur != nullptr);
	CHECK(NumberOf(*exposure, "exposure") == -2.5);
	CHECK(NumberOf(*exposure, "contrast") == 1.125);
	CHECK(NumberOf(*exposure, "gamma") == 8.0);
	CHECK(NumberOf(*hsv, "hue") == -45.25);
	CHECK(NumberOf(*transform, "translate-x") == -0.125);
	CHECK(NumberOf(*transform, "rotation") == -30.5);
	CHECK(NumberOf(*blur, "radius") == 32.0);
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

	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	const ang::NodeInstance *mirror = ByDef(canvas, "render.pass.mirror-capture");
	REQUIRE(mirror != nullptr);
	Set(canvas, mirror->Id, "feedback", ang::Value("last-frame"));
	Set(canvas, mirror->Id, "max-recursion", ang::Value(2.0));

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
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));

	for (const ang::NodeInstance &node : canvas.Nodes()) {
		const auto type = studio::RenderPipelineTypes().FindNode(node.Def);
		REQUIRE(type != nullptr);
		const bool previews = !studio::RenderPipelinePreviewPort(node.Def).empty();
		CHECK((type->FindParam("preview.enabled") != nullptr) == previews);
		CHECK((type->FindParam("preview.reverse-spectrum") != nullptr) == previews);
	}
	REQUIRE(ByDef(canvas, "render.pass.gbuffer") != nullptr);
	const std::string gbufferId = ByDef(canvas, "render.pass.gbuffer")->Id;
	Set(canvas, gbufferId, "preview.enabled", ang::Value(false));
	Set(canvas, gbufferId, "preview.reverse-spectrum", ang::Value(true));

	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	ang::Graph restored;
	REQUIRE(studio::LoadRenderPipelineGraph(saved, restored, error));
	const ang::NodeInstance *gbuffer = ByDef(restored, "render.pass.gbuffer");
	REQUIRE(gbuffer != nullptr);
	CHECK_FALSE(FlagOf(*gbuffer, "preview.enabled"));
	CHECK(FlagOf(*gbuffer, "preview.reverse-spectrum"));
}

TEST_CASE("a canvas edit round trips to a schedulable world document", "[studio][pipeline]") {
	const PipelineDocument basis = DefaultPbrDocument();
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));

	Set(canvas, "ssao", "queue", ang::Value("compute"));
	Set(canvas, "ssao", "async", ang::Value("allow"));

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
	const auto &types = studio::RenderPipelineTypes();
	const auto has = [&](const char *def, std::initializer_list<const char *> params) {
		const auto found = types.FindNode(def);
		REQUIRE(found != nullptr);
		for (const char *param : params) {
			INFO(def << " " << param);
			CHECK(found->FindParam(param) != nullptr);
		}
	};
	ang::Graph blitCanvas;
	const std::string blitId = Add(blitCanvas, "render.pass.blit", {0.0f, 0.0f});
	has("render.pass.blit", {"format"});
	Set(blitCanvas, blitId, "format", ang::Value("RGB10A2"));
	PipelineDocument blitDocument;
	std::string blitError;
	REQUIRE(studio::SaveRenderPipelineGraph(blitCanvas, {}, blitDocument, blitError));
	const auto blitResource =
		std::find_if(blitDocument.Edits().begin(), blitDocument.Edits().end(), [](const Edit &edit) {
			return edit.Kind == EditKind::AddResource;
		});
	REQUIRE(blitResource != blitDocument.Edits().end());
	CHECK(blitResource->Format == ResourceFormat::RGB10A2);

	has("render.pass.dispatch",
		{"dispatch.x",
		 "dispatch.y",
		 "dispatch.z",
		 "dispatch.mode",
		 "local.x",
		 "local.y",
		 "local.z",
		 "shader",
		 "source"});
	has("render.pass.raster", {"shader", "source", "load"});
	ang::Graph rasterCanvas;
	const std::string rasterId = Add(rasterCanvas, "render.pass.raster", {0.0f, 0.0f});
	Set(rasterCanvas, rasterId, "source", ang::Value("#version 450\nvoid main() {}"));
	Set(rasterCanvas, rasterId, "load", ang::Value("load"));
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

	has("render.pass.viewer", {"view"});
	has("render.pass.capture", {"view", "path", "capture.mode"});

	const PipelineDocument basis = DefaultPbrDocument();
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	Set(canvas, "ssao", "resource.occlusion.resolution", ang::Value("quarter"));
	Set(canvas, "ssao", "resource.occlusion.lifetime", ang::Value("external"));

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
	ang::Graph canvas;
	Add(canvas, "render.pass.cull-frustum", {0.0f, 0.0f});
	const std::string distanceId = Add(canvas, "render.pass.cull-distance", {200.0f, 0.0f});
	const std::string tagId = Add(canvas, "render.pass.filter-tag", {400.0f, 0.0f});
	Add(canvas, "render.pass.gbuffer", {600.0f, 0.0f});
	const auto &types = studio::RenderPipelineTypes();
	CHECK(types.FindNode("render.pass.cull-frustum")->FindParam("culling") != nullptr);
	CHECK(types.FindNode("render.pass.cull-distance")->FindParam("radius") != nullptr);
	CHECK(types.FindNode("render.pass.filter-tag")->FindParam("mask") != nullptr);
	CHECK(types.FindNode("render.pass.gbuffer")->FindParam("culling") == nullptr);

	Set(canvas, distanceId, "radius", ang::Value("128.5"));
	Set(canvas, tagId, "mask", ang::Value("0x21"));
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
	ang::Graph canvas;
	const std::string entities = Add(canvas, "render.pass.entities", {0.0f, 0.0f});
	const std::string tonemap = Add(canvas, "render.pass.tonemap", {200.0f, 0.0f});
	CHECK_FALSE(Links(canvas, entities, "entities", tonemap, "colour"));
	const std::string weather = Add(canvas, "render.pass.weather-field", {0.0f, 200.0f});
	CHECK_FALSE(Links(canvas, weather, "field", tonemap, "colour"));
}

TEST_CASE("compute colour admission keeps depth attachment roles distinct", "[studio][pipeline][weather]") {
	ang::Graph canvas;
	const std::string shadow = Add(canvas, "render.pass.shadow", {0.0f, 0.0f});
	const std::string fog = Add(canvas, "render.pass.fog", {200.0f, 0.0f});
	// Both pins are displayed as IMAGE. Save still enforces the engine roles.
	REQUIRE(Links(canvas, shadow, "shadow", fog, "colour"));
	REQUIRE(canvas.Apply(ang::edit::AddLink{{shadow, "shadow", fog, "colour"}}).Ok);
	PipelineDocument saved;
	std::string error;
	CHECK_FALSE(studio::SaveRenderPipelineGraph(canvas, PipelineDocument{}, saved, error));
	CHECK(error.find("depth buffer") != std::string::npos);
}

TEST_CASE("render canvas sockets consume the registered common declarations", "[studio][pipeline]") {
	for (const auto &kind : NodeCatalogue::All()) {
		const auto type = studio::RenderPipelineTypes().FindNode("render.pass." + kind.Declaration.Id);
		REQUIRE(type);
		size_t inputs = 0;
		size_t outputs = 0;
		for (const auto &port : kind.Declaration.Ports) {
			const bool input = port.Direction == engine::graphcore::PortDirection::Input;
			const auto &side = input ? type->Inputs : type->Outputs;
			size_t &index = input ? inputs : outputs;
			REQUIRE(index < side.size());
			CHECK(side[index].Name == port.Id);
			ResourceKind role;
			REQUIRE(ResourceKindFromPortType(port.ValueType, role));
			const bool image = role == ResourceKind::Colour || role == ResourceKind::Depth ||
							   role == ResourceKind::Texture || role == ResourceKind::Storage;
			CHECK(side[index].Type == (image ? "render.image" : port.ValueType));
			++index;
		}
		CHECK(type->Inputs.size() == inputs);
		CHECK(type->Outputs.size() == outputs);
	}
}

TEST_CASE("a renamed pass and a new group save under their edited names", "[studio][pipeline]") {
	const PipelineDocument basis = DefaultPbrDocument();
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	// Ids stay the loaded pass names; the label is what saves.
	Set(canvas, "ssao", studio::RENDER_PIPELINE_LABEL, ang::Value("occlusion-pass"));
	REQUIRE(canvas.Apply(ang::edit::AddGroup{{.Id = "group1", .Label = "post"}}).Ok);
	REQUIRE(canvas.Apply(ang::edit::SetNodeGroup{"bloom", "group1"}).Ok);
	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	CHECK(std::any_of(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::AddNode && edit.Name == engine::core::Name("occlusion-pass");
	}));
	CHECK(std::any_of(saved.Edits().begin(), saved.Edits().end(), [](const Edit &edit) {
		return edit.Kind == EditKind::Group && edit.Name == engine::core::Name("post") &&
			   edit.Target == engine::core::Name("bloom");
	}));
	RenderGraph rebuilt;
	engine::core::Name offender;
	CHECK(Build(saved, rebuilt, offender) == PipelineDocumentStatus::Ok);
}

TEST_CASE("profile numbers become per-pass canvas feedback", "[studio][pipeline]") {
	const PipelineDocument basis = DefaultPbrDocument();
	ang::Graph canvas;
	std::string error;
	REQUIRE(studio::LoadRenderPipelineGraph(basis, canvas, error));
	PipelineDocument saved;
	REQUIRE(studio::SaveRenderPipelineGraph(canvas, basis, saved, error));
	RenderGraph built;
	engine::core::Name offender;
	REQUIRE(Build(saved, built, offender) == PipelineDocumentStatus::Ok);
	CompiledGraph compiled;
	REQUIRE(built.Compile(compiled, offender) == GraphStatus::Ok);
	PipelineProfile profile = ProfilePipeline(built, compiled, 1920, 1080);
	// Only ssao has a measured time: GPU 500 us.
	size_t ssaoIndex = 0;
	for (size_t index = 0; index < canvas.Nodes().size(); index++)
		if (canvas.Nodes()[index].Id == "ssao") ssaoIndex = index;
	for (ProfilePass &pass : profile.Passes)
		if (pass.Node == NodeId{static_cast<uint32_t>(ssaoIndex + 1)}) pass.Elapsed = 500.0;

	ang::Plan plan;
	ang::RunReport report;
	ang::RunTimeline timeline;
	engine::render::Renderer::PipelineRun run;
	run.Milliseconds = 2.0;
	// The frame ran bloom twice, once per view, and refused tonemap.
	run.Nodes = {
		{engine::core::Name("bloom"), ang::NodeStatus::Ran, 0.0, 0.25},
		{engine::core::Name("bloom"), ang::NodeStatus::Ran, 0.5, 0.25},
		{engine::core::Name("tonemap"), ang::NodeStatus::Failed, 1.0, 0.1},
	};
	studio::RenderPipelineFeedback(canvas, built, profile, run, plan, report, timeline);
	REQUIRE(plan.Find("bloom") >= 0);
	const ang::NodeReport &bloom = report.Nodes[static_cast<size_t>(plan.Find("bloom"))];
	CHECK(bloom.Status == ang::NodeStatus::Ran);
	CHECK(bloom.Milliseconds == 0.5);
	REQUIRE(timeline.Latest() != nullptr);
	CHECK(timeline.Latest()->Samples.size() == 3);
	REQUIRE(report.Nodes.size() == canvas.Nodes().size());
	REQUIRE(plan.Find("ssao") == static_cast<int64_t>(ssaoIndex));
	const ang::NodeReport &ssao = report.Nodes[ssaoIndex];
	CHECK(ssao.Status == ang::NodeStatus::Ran);
	CHECK(ssao.Milliseconds == 0.5);
	CHECK(ssao.Bytes > 0);
	const int64_t tonemap = plan.Find("tonemap");
	REQUIRE(tonemap >= 0);
	CHECK(report.Nodes[static_cast<size_t>(tonemap)].Status == ang::NodeStatus::Failed);
	const int64_t gbuffer = plan.Find("gbuffer");
	REQUIRE(gbuffer >= 0);
	CHECK(report.Nodes[static_cast<size_t>(gbuffer)].Status == ang::NodeStatus::Idle);
	CHECK(report.Nodes[static_cast<size_t>(gbuffer)].Bytes > ssao.Bytes);
}

TEST_CASE("the pipeline panels load once per world, not once per frame", "[studio][pipeline]") {
	std::optional<uint32_t> tried;
	CHECK(studio::RenderPipelineNeedsLoad(tried, 0));
	// The load records the world whether it worked or not, so the next frames
	// keep the canvas selection and view instead of reloading over a drag.
	tried = 0;
	CHECK(!studio::RenderPipelineNeedsLoad(tried, 0));
	CHECK(!studio::RenderPipelineNeedsLoad(tried, 0));
	CHECK(studio::RenderPipelineNeedsLoad(tried, 1));
	// No active world is a world change too, tried once like any other.
	tried = engine::world::WorldId::INVALID;
	CHECK(!studio::RenderPipelineNeedsLoad(tried, engine::world::WorldId::INVALID));
}
