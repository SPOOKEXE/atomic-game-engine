// Device-free checks for the renderer's graph installation boundary.
//
// The renderer no longer has a parallel enum or fixed pass list. A pipeline is
// accepted only when every enabled node has a backend implementation. The
// default output path belongs to the engine's default graph, not this boundary.

#include "../src/PipelineCompiler.hpp"
#include "EnvironmentModes.hpp"

#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.render.passes")

// Graph implementation changes must re-run the backend acceptance checks.
TEST_DEPENDS("engine.graph.rendergraph")
TEST_DEPENDS("engine.graph.pipelinedocument")

using engine::core::Name;
using engine::graph::RenderGraph;
using engine::render::FrameResult;
using engine::render::Renderer;

namespace {
	std::vector<std::string>
	NodeNames(const RenderGraph &graph, std::span<const engine::graph::NodeId> nodes) {
		std::vector<std::string> names;
		names.reserve(nodes.size());
		for (const engine::graph::NodeId id : nodes) {
			const engine::graph::Node *node = graph.Find(id);
			REQUIRE(node != nullptr);
			names.emplace_back(node->Name.Text());
		}
		return names;
	}

	std::vector<std::string> ResourceNames(const RenderGraph &graph) {
		std::vector<std::string> names;
		names.reserve(graph.ResourceCount());
		for (uint32_t value = 1; value <= graph.ResourceCount(); ++value) {
			const engine::graph::ResourceDesc *resource =
				graph.FindResource(engine::graph::ResourceId{value});
			REQUIRE(resource != nullptr);
			names.emplace_back(resource->Name.Text());
		}
		return names;
	}

	std::vector<std::string>
	ScheduleNames(const RenderGraph &graph, const engine::graph::ExecutionSchedule &schedule) {
		std::vector<std::string> waves;
		waves.reserve(schedule.Waves.size());
		for (const engine::graph::ExecutionWave &wave : schedule.Waves) {
			std::string signature = wave.Concurrent ? "concurrent" : "serial";
			for (const engine::graph::ScheduledNode &scheduled : wave.Nodes) {
				const engine::graph::Node *node = graph.Find(scheduled.Node);
				REQUIRE(node != nullptr);
				signature += ":";
				signature += node->Name.Text();
				signature += "@";
				signature += engine::graph::Describe(scheduled.Queue);
			}
			waves.push_back(std::move(signature));
		}
		return waves;
	}

	std::vector<std::string> CommandBufferNames(
		const RenderGraph &graph, std::span<const engine::graph::PlannedCommandBuffer> buffers
	) {
		std::vector<std::string> signatures;
		signatures.reserve(buffers.size());
		for (const engine::graph::PlannedCommandBuffer &buffer : buffers) {
			std::string signature(engine::graph::Describe(buffer.Class));
			signature += ":" + std::to_string(buffer.FirstWave) + "-" + std::to_string(buffer.LastWave);
			for (const engine::graph::NodeId id : buffer.Nodes) {
				const engine::graph::Node *node = graph.Find(id);
				REQUIRE(node != nullptr);
				signature += ":";
				signature += node->Name.Text();
			}
			signatures.push_back(std::move(signature));
		}
		return signatures;
	}

	std::vector<std::string>
	AliasOwnerNames(const RenderGraph &graph, const engine::graph::ResourceAliasPlan &aliases) {
		std::vector<std::string> names;
		names.reserve(aliases.Allocations.size());
		for (const engine::graph::ResourceId owner : aliases.Allocations) {
			if (!owner.IsValid()) {
				names.emplace_back("-");
				continue;
			}
			const engine::graph::ResourceDesc *resource = graph.FindResource(owner);
			REQUIRE(resource != nullptr);
			names.emplace_back(resource->Name.Text());
		}
		return names;
	}

	RenderGraph DefaultGraph() {
		RenderGraph graph;
		Name offender;
		REQUIRE(
			engine::graph::Build(engine::graph::DefaultPbrDocument(), graph, offender) ==
			engine::graph::PipelineDocumentStatus::Ok
		);
		return graph;
	}

	engine::graph::PipelineDocument
	WithSetting(const engine::graph::PipelineDocument &source, Name node, Name key, std::string value) {
		engine::graph::PipelineDocument configured;
		bool found = false;
		for (const engine::graph::Edit &edit : source.Edits()) {
			configured.Record(edit);
			if (edit.Kind != engine::graph::EditKind::AddNode || edit.Name != node) {
				continue;
			}
			engine::graph::Edit setting;
			setting.Kind = engine::graph::EditKind::Set;
			setting.Key = key;
			setting.Value = std::move(value);
			configured.Record(std::move(setting));
			found = true;
		}
		REQUIRE(found);
		return configured;
	}
}

TEST_CASE("named render graphs compile before entering the runtime cache", "[render][graph]") {
	Renderer renderer;
	const Name first("main#7");
	const Name second("main#2");

	REQUIRE(renderer.SetPipeline(first, DefaultGraph()));
	REQUIRE(renderer.SetPipeline(second, DefaultGraph()));
	CHECK(renderer.Pipelines() == std::vector<Name>{second, first});
	const auto before = renderer.DescribePipeline(first, 640, 480);
	REQUIRE(before);

	RenderGraph unsupported = DefaultGraph();
	const engine::graph::ResourceId storage = unsupported.AddResource(
		{.Name = Name("custom-storage"), .Kind = engine::graph::ResourceKind::Storage}
	);
	engine::graph::Node node;
	node.Name = Name("custom-compute");
	node.Kind = Name("unsupported-test-node");
	node.Writes = {storage};
	node.Scope = engine::graph::NodeScope::Frame;
	REQUIRE(unsupported.AddNode(node).IsValid());

	// A refusal keeps the complete graph already installed under this key.
	CHECK_FALSE(renderer.SetPipeline(first, unsupported));
	CHECK(renderer.Pipelines() == std::vector<Name>{second, first});
	const auto after = renderer.DescribePipeline(first, 640, 480);
	REQUIRE(after);
	CHECK(after->Revision == before->Revision);
	CHECK(after->Graph.Count() == before->Graph.Count());
	CHECK(after->Compiled.Shared == before->Compiled.Shared);
	CHECK(after->Compiled.PerView == before->Compiled.PerView);
	CHECK(after->Compiled.Final == before->Compiled.Final);
	CHECK(after->Schedule.Waves.size() == before->Schedule.Waves.size());
	CHECK(ScheduleNames(after->Graph, after->Schedule) == ScheduleNames(before->Graph, before->Schedule));
	CHECK(after->Aliases.Allocations == before->Aliases.Allocations);
	CHECK(after->Profile.Passes.size() == before->Profile.Passes.size());
	CHECK(after->Profile.Resources.size() == before->Profile.Resources.size());
	CHECK(after->Profile.PeakBytes == before->Profile.PeakBytes);

	CHECK(renderer.RemovePipeline(first));
	CHECK_FALSE(renderer.RemovePipeline(first));
	CHECK(renderer.Pipelines() == std::vector<Name>{second});

	renderer.ResetPipelines();
	CHECK(renderer.Pipelines().empty());
}

TEST_CASE("pipeline admission reports the failing boundary", "[render][graph][diagnostic]") {
	using engine::render::PipelineAdmissionStage;
	using engine::render::PipelineCompilation;
	using engine::render::PipelineFailure;

	struct Case {
		const char *Label;
		RenderGraph (*Build)();
		bool CheckCapabilities;
		PipelineAdmissionStage Stage;
		Name Offender;
		const char *Reason;
	};
	const std::array cases{
		Case{
			.Label = "graph",
			.Build =
				[] {
					RenderGraph graph;
					engine::graph::Node invalid;
					invalid.Name = Name("invalid-resource");
					invalid.Kind = Name("unknown-kind");
					invalid.Writes = {engine::graph::ResourceId{42}};
					graph.AddNode(std::move(invalid));
					return graph;
				},
			.CheckCapabilities = false,
			.Stage = PipelineAdmissionStage::Graph,
			.Offender = Name("invalid-resource"),
			.Reason = "a node names a resource this graph does not hold",
		},
		Case{
			.Label = "schedule",
			.Build =
				[] {
					RenderGraph graph;
					const auto storage = graph.AddResource({
						.Name = Name("storage"),
						.Kind = engine::graph::ResourceKind::Storage,
					});
					engine::graph::Node invalid;
					invalid.Name = Name("bad-dispatch");
					invalid.Kind = Name("compute");
					invalid.Writes = {storage};
					invalid.Parameters = {{Name("dispatch.x"), "zero"}};
					graph.AddNode(std::move(invalid));
					return graph;
				},
			.CheckCapabilities = false,
			.Stage = PipelineAdmissionStage::Schedule,
			.Offender = Name("bad-dispatch"),
			.Reason = "a scheduling hint is not valid",
		},
		Case{
			.Label = "backend",
			.Build =
				[] {
					RenderGraph graph;
					const auto image = graph.AddResource({
						.Name = Name("image"),
						.Kind = engine::graph::ResourceKind::Colour,
					});
					engine::graph::Node unsupported;
					unsupported.Name = Name("unsupported-node");
					unsupported.Kind = Name("unsupported-test-kind");
					unsupported.Writes = {image};
					graph.AddNode(std::move(unsupported));
					return graph;
				},
			.CheckCapabilities = false,
			.Stage = PipelineAdmissionStage::Backend,
			.Offender = Name("unsupported-test-kind"),
			.Reason = "the renderer has no backend node for this kind",
		},
		Case{
			.Label = "capability",
			.Build =
				[] {
					RenderGraph graph;
					const auto source = graph.AddResource({
						.Name = Name("source"),
						.Kind = engine::graph::ResourceKind::Texture,
						.Format = engine::graph::ResourceFormat::RGBA16F,
						.External = true,
					});
					const auto target = graph.AddResource({
						.Name = Name("target"),
						.Kind = engine::graph::ResourceKind::Colour,
						.Format = engine::graph::ResourceFormat::RGBA16F,
					});
					engine::graph::Node blit;
					blit.Name = Name("format-limited-blit");
					blit.Kind = Name("blit");
					blit.Reads = {source};
					blit.Writes = {target};
					graph.AddNode(std::move(blit));
					return graph;
				},
			.CheckCapabilities = true,
			.Stage = PipelineAdmissionStage::Capability,
			.Offender = Name("format-limited-blit"),
			.Reason = "the device does not support a required texture format: RGBA16F",
		},
	};

	const engine::render::DeviceCaps noCapabilities;
	for (const Case &test : cases) {
		INFO(test.Label);
		PipelineCompilation compilation = engine::render::CompilePipeline(
			Name(test.Label), test.Build(), test.CheckCapabilities ? &noCapabilities : nullptr, {}
		);
		REQUIRE_FALSE(compilation);
		const PipelineFailure &failure = compilation.Failure;
		CHECK(failure.Stage == test.Stage);
		CHECK(failure.Offender == test.Offender);
		CHECK(failure.Reason == test.Reason);
	}
}

TEST_CASE(
	"document admission keeps compiler diagnostics and the prior install", "[render][graph][diagnostic]"
) {
	using engine::graph::Edit;
	using engine::graph::EditKind;
	using engine::graph::PipelineDocument;
	using engine::graph::PipelineDocumentStatus;
	using engine::graph::ResourceKind;
	using engine::render::PipelineAdmissionStage;

	PipelineDocument document;
	document.Record(
		Edit{
			.Kind = EditKind::AddResource,
			.Name = Name("storage"),
			.Resource = ResourceKind::Storage,
		}
	);
	document.Record(
		Edit{
			.Kind = EditKind::AddNode,
			.Name = Name("bad-dispatch"),
			.NodeKind = Name("compute"),
		}
	);
	document.Record(Edit{.Kind = EditKind::Writes, .Target = Name("storage")});
	document.Record(
		Edit{
			.Kind = EditKind::Set,
			.Key = Name("dispatch.x"),
			.Value = "zero",
		}
	);

	RenderGraph graph;
	Name graphOffender;
	CHECK(engine::graph::Build(document, graph, graphOffender) == PipelineDocumentStatus::Invalid);
	CHECK(graphOffender == Name("bad-dispatch"));
	engine::graph::ExecutionSchedule schedule;
	Name scheduleOffender;
	const engine::graph::ScheduleStatus scheduleStatus =
		engine::graph::CompileSchedule(graph, schedule, scheduleOffender);
	CHECK(scheduleStatus == engine::graph::ScheduleStatus::InvalidHint);
	CHECK(scheduleOffender == Name("bad-dispatch"));
	CHECK(engine::graph::Describe(scheduleStatus) == std::string("a scheduling hint is not valid"));
	const engine::render::PipelineCompilation compiled =
		engine::render::CompilePipeline(Name("bad document"), graph, nullptr, {});
	REQUIRE_FALSE(compiled);
	CHECK(compiled.Failure.Stage == PipelineAdmissionStage::Schedule);
	CHECK(compiled.Failure.Offender == Name("bad-dispatch"));
	CHECK(compiled.Failure.Reason == "a scheduling hint is not valid");

	Renderer renderer;
	const engine::render::PipelineAdmissionResult validation = renderer.ValidatePipelineDocument(document);
	REQUIRE_FALSE(validation);
	REQUIRE(validation.Failure.has_value());
	CHECK(validation.Failure->Stage == compiled.Failure.Stage);
	CHECK(validation.Failure->Offender == compiled.Failure.Offender);
	CHECK(validation.Failure->Reason == compiled.Failure.Reason);
	CHECK(
		engine::render::FormatPipelineFailure(*validation.Failure) ==
		"refused during schedule: a scheduling hint is not valid at 'bad-dispatch'"
	);
	const engine::render::PipelineAdmissionResult direct =
		renderer.SetPipelineWithResult(Name("direct refusal"), graph);
	REQUIRE_FALSE(direct);
	REQUIRE(direct.Failure.has_value());
	CHECK(direct.Failure->Stage == validation.Failure->Stage);
	CHECK(direct.Failure->Offender == validation.Failure->Offender);
	CHECK(direct.Failure->Reason == validation.Failure->Reason);

	const Name installedName("replace-me");
	REQUIRE(renderer.SetPipeline(installedName, DefaultGraph()));
	const auto before = renderer.DescribePipeline(installedName, 320, 240);
	REQUIRE(before);
	const engine::render::PipelineAdmissionResult replacement =
		renderer.SetPipelineDocument(installedName, document);
	REQUIRE_FALSE(replacement);
	REQUIRE(replacement.Failure.has_value());
	CHECK(replacement.Failure->Stage == validation.Failure->Stage);
	CHECK(replacement.Failure->Offender == validation.Failure->Offender);
	CHECK(replacement.Failure->Reason == validation.Failure->Reason);
	const auto after = renderer.DescribePipeline(installedName, 320, 240);
	REQUIRE(after);
	CHECK(after->Revision == before->Revision);
	CHECK(after->Graph.Count() == before->Graph.Count());
}

TEST_CASE("the default PBR graph compiles into the graph backend", "[render][graph]") {
	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("Default PBR#1"), DefaultGraph()));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("Default PBR#1")});
}

TEST_CASE("default and named PBR installs have the same execution plan", "[render][graph]") {
	Renderer renderer;
	const auto builtIn = renderer.DescribePipeline(Name("Engine Default"), 640, 480);
	REQUIRE(builtIn);
	REQUIRE(renderer.SetPipeline(Name("PBR copy"), DefaultGraph()));
	const auto named = renderer.DescribePipeline(Name("PBR copy"), 640, 480);
	REQUIRE(named);
	CHECK(named->Revision != builtIn->Revision);
	CHECK(named->Graph.Count() == builtIn->Graph.Count());
	CHECK(named->Graph.ResourceCount() == builtIn->Graph.ResourceCount());
	CHECK(named->Compiled.Shared == builtIn->Compiled.Shared);
	CHECK(named->Compiled.PerView == builtIn->Compiled.PerView);
	CHECK(named->Compiled.Final == builtIn->Compiled.Final);
	CHECK(named->Aliases.Allocations == builtIn->Aliases.Allocations);
	REQUIRE(named->Schedule.Waves.size() == builtIn->Schedule.Waves.size());
	for (size_t index = 0; index < named->Schedule.Waves.size(); ++index) {
		const auto &actual = named->Schedule.Waves[index];
		const auto &expected = builtIn->Schedule.Waves[index];
		REQUIRE(actual.Nodes.size() == expected.Nodes.size());
		for (size_t node = 0; node < actual.Nodes.size(); ++node) {
			CHECK(actual.Nodes[node].Node == expected.Nodes[node].Node);
			CHECK(actual.Nodes[node].Queue == expected.Nodes[node].Queue);
		}
	}
	REQUIRE(named->Profile.Passes.size() == builtIn->Profile.Passes.size());
	for (size_t index = 0; index < named->Profile.Passes.size(); ++index) {
		CHECK(named->Profile.Passes[index].Name == builtIn->Profile.Passes[index].Name);
		CHECK(named->Profile.Passes[index].Kind == builtIn->Profile.Passes[index].Kind);
	}
}

TEST_CASE("the default PBR install matches its fixed graph baseline", "[render][graph][characterization]") {
	using namespace engine::graph;
	Renderer renderer;
	const auto installed = renderer.DescribePipeline(Name("Engine Default"), 640, 480);
	REQUIRE(installed);
	CHECK(installed->Pipeline == Name("Engine Default"));
	CHECK(installed->Revision > 0);

	const engine::render::PipelineCompilation compilation =
		engine::render::CompilePipeline(Name("Engine Default"), DefaultGraph(), nullptr, {});
	REQUIRE(compilation);
	const engine::render::InstalledPipeline &package = *compilation.Package;
	CHECK(package.Name == Name("Engine Default"));
	CHECK(package.RetainedNodes.size() == package.Graph.Count());
	std::vector<std::string> retainedNames;
	for (uint32_t value = 1; value <= package.Graph.Count(); ++value) {
		if (package.RetainedNodes[value - 1] == 0) continue;
		const Node *node = package.Graph.Find(NodeId{value});
		REQUIRE(node != nullptr);
		retainedNames.emplace_back(node->Name.Text());
	}
	CHECK(retainedNames == std::vector<std::string>{"present", "overlay", "output-image"});
	CHECK(package.RetainedFamilies == 0);

	const std::vector<std::string> expectedResources{
		"shadow",
		"last-frame",
		"mirror-views",
		"portal-image",
		"portal-light",
		"world-entities",
		"view-camera",
		"view-entities",
		"visible-entities",
		"ordered-entities",
		"resident-meshes",
		"environment-sky",
		"environment-clouds",
		"view-instances",
		"lod-instances",
		"albedo",
		"normal",
		"material",
		"emissive",
		"mesh-uv",
		"object-ids",
		"semantic-ids",
		"part-ids",
		"depth",
		"linear-depth",
		"second-surface-z",
		"second-surface-depth",
		"second-surface-validity",
		"occlusion",
		"lit",
		"sky-lit",
		"volume-lit",
		"lens-b",
		"lens-scratch",
		"dof",
		"god-rays",
		"bloom",
		"tonemapped",
		"portaled",
		"mirrored",
		"display",
		"scene-image",
		"interface-image",
		"composed-image",
	};
	CHECK(ResourceNames(installed->Graph) == expectedResources);

	const std::vector<std::string> authoredNodes{
		"world",
		"mesh-residency",
		"shadow",
		"skybox-compute",
		"clouds-compute",
		"camera",
		"last-frame",
		"entities",
		"cull-frustum",
		"order-draw",
		"delta-upload",
		"select-lod",
		"surface-capture",
		"gbuffer",
		"depth-peel",
		"depth-linearise",
		"ssao",
		"deferred-lighting",
		"sky",
		"fog",
		"portal-overlay",
		"mirror-overlay",
		"transparent",
		"shader-lenses",
		"dof",
		"god-rays",
		"bloom",
		"tonemap",
		"present",
		"interface",
		"overlay",
		"output-image",
	};
	std::vector<std::string> actualAuthoredNodes;
	for (uint32_t value = 1; value <= installed->Graph.Count(); ++value) {
		const Node *node = installed->Graph.Find(NodeId{value});
		REQUIRE(node != nullptr);
		actualAuthoredNodes.emplace_back(node->Name.Text());
	}
	CHECK(actualAuthoredNodes == authoredNodes);
	const Node *disabledDepthPeel = installed->Graph.Find(NodeId{15});
	REQUIRE(disabledDepthPeel != nullptr);
	CHECK_FALSE(disabledDepthPeel->Enabled);

	const std::vector<std::string> expectedShared{
		"world",
		"mesh-residency",
		"skybox-compute",
		"shadow",
		"clouds-compute",
	};
	const std::vector<std::string> expectedPerView{
		"camera",
		"last-frame",
		"entities",
		"cull-frustum",
		"order-draw",
		"delta-upload",
		"select-lod",
		"surface-capture",
		"gbuffer",
		"depth-linearise",
		"ssao",
		"deferred-lighting",
		"sky",
		"fog",
		"portal-overlay",
		"mirror-overlay",
		"transparent",
		"shader-lenses",
		"dof",
		"god-rays",
		"bloom",
		"tonemap",
	};
	const std::vector<std::string> expectedFinal{"present", "interface", "overlay", "output-image"};
	CHECK(NodeNames(installed->Graph, installed->Compiled.Shared) == expectedShared);
	CHECK(NodeNames(installed->Graph, installed->Compiled.PerView) == expectedPerView);
	CHECK(NodeNames(installed->Graph, installed->Compiled.Final) == expectedFinal);

	const PipelineProfile &profile = installed->Profile;
	const std::vector<std::string> expectedPasses{
		"world",
		"mesh-residency",
		"skybox-compute",
		"shadow",
		"clouds-compute",
		"camera",
		"last-frame",
		"entities",
		"cull-frustum",
		"order-draw",
		"delta-upload",
		"select-lod",
		"surface-capture",
		"gbuffer",
		"depth-linearise",
		"ssao",
		"deferred-lighting",
		"sky",
		"fog",
		"portal-overlay",
		"mirror-overlay",
		"transparent",
		"shader-lenses",
		"dof",
		"god-rays",
		"bloom",
		"tonemap",
		"present",
		"interface",
		"overlay",
		"output-image",
	};
	REQUIRE(profile.Passes.size() == expectedPasses.size());
	CHECK(profile.Cells.size() == profile.Resources.size() * profile.Passes.size());
	REQUIRE(profile.Resources.size() == expectedResources.size());
	for (size_t index = 0; index < profile.Passes.size(); ++index) {
		const ProfilePass &pass = profile.Passes[index];
		const Node *node = installed->Graph.Find(pass.Node);
		REQUIRE(node != nullptr);
		CHECK(pass.Name == Name(expectedPasses[index]));
		CHECK(pass.Name == node->Name);
		CHECK(pass.Kind == node->Kind);
		CHECK(pass.Where == (index < 5 ? Band::Shared : (index < 27 ? Band::PerView : Band::Final)));
		CHECK(pass.Elapsed == 0.0);
		CHECK(pass.Wall == 0.0);
	}
	for (size_t index = 0; index < profile.Resources.size(); ++index) {
		const ProfileResource &resource = profile.Resources[index];
		CHECK(resource.Id.Value == index + 1);
		CHECK(resource.Name == Name(expectedResources[index]));
		CHECK(resource.Allocation == installed->Aliases.AllocationOf(resource.Id));
	}
	CHECK(profile.At(15, 13) == Access::Write);
	CHECK(profile.At(23, 13) == Access::Write);
	CHECK(profile.At(23, 14) == Access::Read);
	CHECK(profile.At(24, 15) == Access::Read);
	CHECK(profile.At(42, 28) == Access::Write);
	CHECK(profile.At(43, 30) == Access::Read);
	CHECK(profile.Resources[11].Width == 1024);
	CHECK(profile.Resources[11].Height == 512);
	CHECK(profile.Resources[28].Width == 320);
	CHECK(profile.Resources[28].Height == 240);
	CHECK(profile.PeakBytes == 32'119'808);
	CHECK(profile.TotalBytes == 67'447'808);
	CHECK(profile.AllocatedBytes == 23'347'200);
	CHECK(profile.AliasedResources == 9);

	CHECK(installed->Aliases.Allocations.size() == installed->Graph.ResourceCount());
	CHECK(installed->Aliases.PhysicalTargets == 15);
	CHECK(installed->Aliases.AliasedResources == 9);
	const std::vector<std::string> expectedAliases{
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"albedo",
		"normal",
		"material",
		"emissive",
		"mesh-uv",
		"object-ids",
		"semantic-ids",
		"part-ids",
		"depth",
		"linear-depth",
		"-",
		"-",
		"-",
		"-",
		"lit",
		"emissive",
		"lit",
		"emissive",
		"lit",
		"lit",
		"emissive",
		"lit",
		"albedo",
		"portaled",
		"mirrored",
		"portaled",
		"scene-image",
		"-",
		"composed-image",
	};
	CHECK(AliasOwnerNames(installed->Graph, installed->Aliases) == expectedAliases);
	const std::vector<std::string> expectedWaves{
		"concurrent:world@cpu:mesh-residency@transfer:skybox-compute@compute",
		"concurrent:shadow@graphics:clouds-compute@compute",
		"serial:camera@cpu:last-frame@graphics:entities@cpu",
		"serial:cull-frustum@cpu",
		"serial:order-draw@cpu",
		"serial:delta-upload@transfer",
		"serial:select-lod@compute",
		"serial:surface-capture@graphics:gbuffer@graphics",
		"serial:depth-linearise@graphics",
		"serial:ssao@graphics",
		"serial:deferred-lighting@graphics",
		"serial:sky@graphics",
		"serial:fog@graphics",
		"serial:portal-overlay@graphics",
		"serial:mirror-overlay@graphics",
		"serial:transparent@graphics",
		"serial:shader-lenses@graphics",
		"serial:dof@graphics",
		"serial:god-rays@graphics",
		"serial:bloom@graphics",
		"serial:tonemap@graphics",
		"serial:present@graphics:interface@graphics",
		"serial:overlay@graphics",
		"serial:output-image@transfer",
	};
	CHECK(ScheduleNames(installed->Graph, installed->Schedule) == expectedWaves);
	const std::vector<std::string> expectedBuffers{
		"transfer:0-0:mesh-residency",
		"compute:0-1:skybox-compute:clouds-compute",
		"graphics:1-2:shadow:last-frame",
		"transfer:5-5:delta-upload",
		"compute:6-6:select-lod",
		"graphics:7-22:surface-capture:gbuffer:depth-linearise:ssao:deferred-lighting:sky:fog:portal-overlay:"
		"mirror-overlay:transparent:shader-lenses:dof:god-rays:bloom:tonemap:present:interface:overlay",
		"transfer:23-23:output-image",
	};
	const std::vector<PlannedCommandBuffer> replannedBuffers = PlanCommandBuffers(package.Schedule);
	REQUIRE_FALSE(package.Buffers.empty());
	CHECK(CommandBufferNames(package.Graph, package.Buffers) == expectedBuffers);
	CHECK(CommandBufferNames(package.Graph, replannedBuffers) == expectedBuffers);
	CHECK(ScheduleNames(package.Graph, package.Schedule) == expectedWaves);
	for (const PlannedCommandBuffer &buffer : package.Buffers) {
		CHECK(buffer.FirstWave <= buffer.LastWave);
		CHECK(buffer.LastWave < package.Schedule.Waves.size());
	}
}

TEST_CASE("render graph snapshots require an exact installed name", "[render][graph][diagnostic]") {
	Renderer renderer;
	CHECK_FALSE(renderer.DescribePipeline(Name("not-installed"), 640, 480));
	CHECK_FALSE(renderer.DescribePipeline(Name("Engine Default"), 0, 480));

	const auto snapshot = renderer.DescribePipeline(Name("Engine Default"), 640, 480);
	REQUIRE(snapshot);
	CHECK(snapshot->Pipeline == Name("Engine Default"));
	CHECK_FALSE(snapshot->Profile.Passes.empty());
	CHECK(snapshot->Aliases.Allocations.size() == snapshot->Graph.ResourceCount());

	for (const engine::graph::ProfilePass &pass : snapshot->Profile.Passes) {
		const auto *node = snapshot->Graph.Find(pass.Node);
		REQUIRE(node != nullptr);
		CHECK(pass.Name == node->Name);
		CHECK(pass.Kind == node->Kind);
	}
	for (uint32_t value = 1; value <= snapshot->Graph.ResourceCount(); ++value) {
		const engine::graph::ResourceId resource{value};
		const auto allocation = snapshot->Aliases.AllocationOf(resource);
		if (allocation.IsValid()) {
			CHECK(snapshot->Graph.FindResource(allocation) != nullptr);
		}
	}

	bool hasHistory = false;
	for (uint32_t value = 1; value <= snapshot->Graph.ResourceCount(); ++value) {
		const auto *resource = snapshot->Graph.FindResource(engine::graph::ResourceId{value});
		hasHistory = hasHistory ||
					 (resource != nullptr && resource->Lifetime == engine::graph::ResourceLifetime::History);
	}
	CHECK(hasHistory);
}

TEST_CASE("hard render demo graphs install compute handlers", "[render][graph][hard-render]") {
	for (const auto &[name, document] : std::array{
			 std::pair{Name("Raytrace Demo"), engine::graph::RaytraceDemoDocument()},
			 std::pair{Name("Pathtrace Demo"), engine::graph::PathtraceDemoDocument()},
		 }) {
		RenderGraph graph;
		Name offender;
		REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);

		Renderer renderer;
		CHECK(renderer.SetPipeline(name, graph));
	}
}

TEST_CASE(
	"retained shadow correction requires its complete input group", "[render][graph][directional-correct]"
) {
	using namespace engine::graph;
	const auto base = DefaultPortalBodyDocument(false, true, true, true, false, 2, true);
	for (unsigned mask = 0; mask < 16; ++mask) {
		CAPTURE(mask);
		PipelineDocument document;
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = Name("room-directional-response"),
			 .Resource = ResourceKind::Colour,
			 .Format = ResourceFormat::RGBA32F}
		);
		Name current;
		for (const auto &edit : base.Edits()) {
			if (edit.Kind == EditKind::AddNode) current = edit.Name;
			document.Record(edit);
			if (current == Name("room-image") && edit.Kind == EditKind::Writes &&
				edit.Key == Name("lighting-baseline"))
				document.Record(
					{.Kind = EditKind::Writes,
					 .Target = Name("room-directional-response"),
					 .Key = Name("directional-response")}
				);
			if (current == Name("ambient-correct") && edit.Kind == EditKind::Reads &&
				edit.Key == Name("occlusion")) {
				const char *ports[]{"directional-response", "room-depth", "room-normal", "shadow"};
				const char *resources[]{"room-directional-response", "room-depth", "room-normal", "shadow"};
				for (unsigned input = 0; input < 4; ++input)
					if (mask & (1u << input))
						document.Record(
							{.Kind = EditKind::Reads,
							 .Target = Name(resources[input]),
							 .Key = Name(ports[input])}
						);
			}
		}
		RenderGraph graph;
		Name offender;
		REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
		PipelineDocument restored;
		REQUIRE(Read(Write(document), restored, offender) == PipelineDocumentStatus::Ok);
		CHECK(Write(restored) == Write(document));
		Renderer renderer;
		CHECK(renderer.SetPipeline(Name("retained-shadow-inputs"), graph) == (mask == 0 || mask == 15));
	}
}

TEST_CASE("depth exports preserve the lighting depth singleton", "[render][graph][depth-export]") {
	using namespace engine::graph;
	for (const bool zeroBackground : {false, true}) {
		const auto base = DefaultPbrDocument();
		PipelineDocument document;
		for (const auto &edit : base.Edits()) {
			if (edit.Kind == EditKind::AddNode && edit.Name == Name("present")) {
				document.Record(
					{.Kind = EditKind::AddResource,
					 .Name = Name("export-depth"),
					 .Resource = ResourceKind::Colour,
					 .Format = ResourceFormat::R32F}
				);
				document.Record(
					{.Kind = EditKind::AddNode,
					 .Name = Name("export-linearise"),
					 .NodeKind = Name("depth-linearise"),
					 .Scope = NodeScope::View}
				);
				document.Record({.Kind = EditKind::Reads, .Target = Name("depth"), .Key = Name("depth")});
				document.Record(
					{.Kind = EditKind::Writes, .Target = Name("export-depth"), .Key = Name("linear")}
				);
				document.Record(
					{.Kind = EditKind::Set,
					 .Key = Name("background"),
					 .Value = zeroBackground ? "zero" : "far"}
				);
			}
			document.Record(edit);
		}
		RenderGraph graph;
		Name offender;
		REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
		Renderer renderer;
		CHECK(renderer.SetPipeline(Name("export#1"), graph) == zeroBackground);
	}
}

TEST_CASE("built-in capability fallbacks compile into the graph backend", "[render][graph]") {
	Renderer renderer;
	for (const auto &[name, document] : {
			 std::pair{Name("Eye#1"), engine::graph::DefaultEyeDocument()},
			 std::pair{Name("Tier B#1"), engine::graph::DefaultPbrTierBDocument()},
			 std::pair{Name("Tier C#1"), engine::graph::DefaultForwardTierCDocument()},
		 }) {
		RenderGraph graph;
		Name offender;
		REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
		if (name == Name("Eye#1")) {
			const auto readTarget = [&graph](Name kind, Name port) -> Name {
				for (uint32_t value = 1; value <= graph.Count(); ++value) {
					const auto *node = graph.Find(engine::graph::NodeId{value});
					if (node == nullptr || node->Kind != kind) continue;
					const auto found = std::find(node->ReadPorts.begin(), node->ReadPorts.end(), port);
					if (found == node->ReadPorts.end()) return {};
					const size_t index = static_cast<size_t>(found - node->ReadPorts.begin());
					const auto *resource = graph.FindResource(node->Reads[index]);
					return resource == nullptr ? Name{} : resource->Name;
				}
				return {};
			};
			CHECK(readTarget(Name("bloom"), Name("source")) == Name("eye-hdr"));
			CHECK(readTarget(Name("tonemap"), Name("colour")) == Name("eye-hdr"));
		}
		CHECK(renderer.SetPipeline(name, graph));
	}
}

TEST_CASE("default PBR stages are not mandatory backend policy", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId display = graph.AddResource(
		{.Name = Name("display"), .Kind = engine::graph::ResourceKind::Colour, .External = true}
	);
	REQUIRE(display.IsValid());

	engine::graph::Node present;
	present.Name = Name("minimal-present");
	present.Kind = Name("present");
	present.Reads = {display};
	present.Scope = engine::graph::NodeScope::Frame;
	REQUIRE(graph.AddNode(present).IsValid());

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("minimal#1"), graph));
}

TEST_CASE("draw uploads are graph dependencies rather than backend fallbacks", "[render][graph][uploads]") {
	using engine::graph::Node;
	using engine::graph::NodeScope;
	using engine::graph::ResourceKind;

	const auto make = [](bool residency, bool upload, bool uploadFirst) {
		RenderGraph graph;
		const auto meshes =
			graph.AddResource({.Name = Name("meshes"), .Kind = ResourceKind::Buffer, .External = !residency});
		const auto entities =
			graph.AddResource({.Name = Name("entities"), .Kind = ResourceKind::Entities, .External = true});
		const auto instances =
			graph.AddResource({.Name = Name("instances"), .Kind = ResourceKind::Buffer, .External = !upload});
		const auto colour = graph.AddResource({.Name = Name("colour"), .Kind = ResourceKind::Colour});
		if (residency) {
			graph.AddNode({
				.Name = Name("mesh-residency"),
				.Kind = Name("mesh-residency"),
				.Writes = {meshes},
				.Scope = NodeScope::World,
			});
		}
		const auto addUpload = [&] {
			if (!upload) return;
			graph.AddNode({
				.Name = Name("delta-upload"),
				.Kind = Name("delta-upload"),
				.Reads = {meshes, entities},
				.Writes = {instances},
				.Scope = NodeScope::View,
			});
		};
		const auto addDraw = [&] {
			graph.AddNode({
				.Name = Name("transparent"),
				.Kind = Name("transparent"),
				.Reads = {instances},
				.Writes = {colour},
				.Scope = NodeScope::View,
			});
		};
		if (uploadFirst) addUpload();
		addDraw();
		if (!uploadFirst) addUpload();
		return graph;
	};

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("ordered#1"), make(true, true, true)));
	CHECK_FALSE(renderer.SetPipeline(Name("missing-residency#1"), make(false, true, true)));
	CHECK_FALSE(renderer.SetPipeline(Name("missing-upload#1"), make(true, false, true)));
	CHECK_FALSE(renderer.SetPipeline(Name("late-upload#1"), make(true, true, false)));
}

TEST_CASE("authored raster compute and inspection nodes are repeatable backend work", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId source = graph.AddResource(
		{.Name = Name("source"), .Kind = engine::graph::ResourceKind::Texture, .External = true}
	);
	const engine::graph::ResourceId first =
		graph.AddResource({.Name = Name("first"), .Kind = engine::graph::ResourceKind::Colour});
	const engine::graph::ResourceId second =
		graph.AddResource({.Name = Name("second"), .Kind = engine::graph::ResourceKind::Colour});
	const engine::graph::ResourceId storage =
		graph.AddResource({.Name = Name("storage"), .Kind = engine::graph::ResourceKind::Storage});
	REQUIRE(source.IsValid());
	REQUIRE(first.IsValid());
	REQUIRE(second.IsValid());
	REQUIRE(storage.IsValid());

	engine::graph::Node raster;
	raster.Name = Name("first-raster");
	raster.Kind = Name("raster");
	raster.Reads = {source};
	raster.Writes = {first};
	raster.Scope = engine::graph::NodeScope::View;
	REQUIRE(graph.AddNode(raster).IsValid());
	raster.Name = Name("second-raster");
	raster.Reads = {first};
	raster.Writes = {second};
	REQUIRE(graph.AddNode(raster).IsValid());

	engine::graph::Node dispatch;
	dispatch.Name = Name("compute");
	dispatch.Kind = Name("dispatch");
	dispatch.Reads = {second};
	dispatch.Writes = {storage};
	dispatch.Scope = engine::graph::NodeScope::View;
	REQUIRE(graph.AddNode(dispatch).IsValid());

	for (const char *kind : {"viewer", "capture", "viewer"}) {
		engine::graph::Node sink;
		sink.Name = Name(std::string(kind) + std::to_string(graph.Count()));
		sink.Kind = Name(kind);
		sink.Reads = {storage};
		sink.Scope = engine::graph::NodeScope::Frame;
		REQUIRE(graph.AddNode(sink).IsValid());
	}

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("authored#1"), graph));
}

TEST_CASE("authored compute can be scoped once per world", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId storage =
		graph.AddResource({.Name = Name("world-storage"), .Kind = engine::graph::ResourceKind::Storage});
	REQUIRE(storage.IsValid());

	engine::graph::Node dispatch;
	dispatch.Name = Name("world-compute");
	dispatch.Kind = Name("dispatch");
	dispatch.Writes = {storage};
	dispatch.Scope = engine::graph::NodeScope::World;
	REQUIRE(graph.AddNode(dispatch).IsValid());

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("world-compute#1"), graph));
}

TEST_CASE("frame setup can feed a view before final frame inspection", "[render][graph][frame-prefix]") {
	RenderGraph graph;
	const auto resident =
		graph.AddResource({.Name = Name("resident"), .Kind = engine::graph::ResourceKind::Storage});
	const auto visible =
		graph.AddResource({.Name = Name("visible"), .Kind = engine::graph::ResourceKind::Storage});
	graph.AddNode({
		.Name = Name("setup"),
		.Kind = Name("dispatch"),
		.Writes = {resident},
		.Scope = engine::graph::NodeScope::Frame,
	});
	graph.AddNode({
		.Name = Name("view"),
		.Kind = Name("dispatch"),
		.Reads = {resident},
		.Writes = {visible},
		.Scope = engine::graph::NodeScope::View,
	});
	graph.AddNode({
		.Name = Name("inspection"),
		.Kind = Name("viewer"),
		.Reads = {visible},
		.Scope = engine::graph::NodeScope::Frame,
	});
	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("frame-setup#1"), graph));
}

TEST_CASE("blit owns its target format at the installation boundary", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId source = graph.AddResource(
		{.Name = Name("source"),
		 .Kind = engine::graph::ResourceKind::Texture,
		 .Format = engine::graph::ResourceFormat::RGBA16F,
		 .External = true}
	);
	const engine::graph::ResourceId target = graph.AddResource(
		{.Name = Name("target"),
		 .Kind = engine::graph::ResourceKind::Colour,
		 .Format = engine::graph::ResourceFormat::RGB10A2,
		 .External = true}
	);
	REQUIRE(source.IsValid());
	REQUIRE(target.IsValid());

	engine::graph::Node blit;
	blit.Name = Name("convert");
	blit.Kind = Name("blit");
	blit.Reads = {source};
	blit.Writes = {target};
	blit.Scope = engine::graph::NodeScope::View;
	blit.Parameters.push_back({.Key = Name("format"), .Value = "RGB10A2"});
	REQUIRE(graph.AddNode(blit).IsValid());

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("conversion#1"), graph));

	blit.Name = Name("mismatch");
	blit.Parameters.front().Value = "RGBA8";
	RenderGraph mismatched;
	const engine::graph::ResourceId mismatchSource = mismatched.AddResource(
		{.Name = Name("source"), .Kind = engine::graph::ResourceKind::Texture, .External = true}
	);
	const engine::graph::ResourceId mismatchTarget = mismatched.AddResource(
		{.Name = Name("target"),
		 .Kind = engine::graph::ResourceKind::Colour,
		 .Format = engine::graph::ResourceFormat::RGB10A2,
		 .External = true}
	);
	blit.Reads = {mismatchSource};
	blit.Writes = {mismatchTarget};
	REQUIRE(mismatched.AddNode(blit).IsValid());
	CHECK_FALSE(renderer.SetPipeline(Name("conversion#2"), mismatched));
}

TEST_CASE("environment enabled switches select only their authored GPU mode", "[render][environment]") {
	engine::scene::Environment environment;
	environment.Skybox = engine::scene::SkyboxSource::Textures;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 1);
	environment.Textures.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 0);

	environment.Skybox = engine::scene::SkyboxSource::Compute;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 2);
	environment.SkyCompute.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 0);

	environment.HasClouds = true;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 1);
	environment.HasCloudCompute = true;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 2);
	environment.CloudVolume.Shader = engine::scene::CloudComputeShader::Voxel;
	CHECK(
		engine::render::EnvironmentShadersOf(environment).Clouds ==
		static_cast<uint32_t>(engine::scene::CloudComputeShader::Voxel)
	);
	environment.CloudVolume.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 0);
	CHECK(engine::render::EnvironmentShadersOf(environment).Clouds == 0);
	CHECK(
		engine::render::EnvironmentCloudComputeOf(environment).Shader ==
		engine::scene::CloudComputeShader::Cumulus
	);
	environment.CloudLayer.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 0);

	environment.HasAtmosphere = true;
	CHECK(engine::render::EnvironmentModesOf(environment).Atmosphere == 1);
	environment.HasAtmosphereCompute = true;
	environment.AirCompute.Shader = engine::scene::AtmosphereProceduralShader::Alien;
	CHECK(engine::render::EnvironmentModesOf(environment).Atmosphere == 2);
	CHECK(
		engine::render::EnvironmentShadersOf(environment).Atmosphere ==
		static_cast<uint32_t>(engine::scene::AtmosphereProceduralShader::Alien)
	);
	environment.AirCompute.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Atmosphere == 0);
	CHECK(engine::render::EnvironmentShadersOf(environment).Atmosphere == 0);
	CHECK(
		engine::render::EnvironmentAtmosphereComputeOf(environment).Shader ==
		engine::scene::AtmosphereProceduralShader::Earth
	);
}

TEST_CASE("optional default nodes can be disabled at the backend boundary", "[render][graph]") {
	engine::graph::PipelineDocument document = engine::graph::DefaultPbrDocument();
	engine::graph::Edit disabled;
	disabled.Kind = engine::graph::EditKind::Enable;
	disabled.Name = Name("shadow");
	disabled.Enabled = false;
	document.Record(disabled);
	disabled.Name = Name("ssao");
	document.Record(disabled);
	disabled.Name = Name("surface-capture");
	document.Record(disabled);

	RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("unshadowed#1"), graph));
}

TEST_CASE("backend queue and overlap controls describe the work each node records", "[render][graph]") {
	engine::graph::PipelineDocument document = engine::graph::DefaultPbrDocument();
	document = WithSetting(document, Name("cull-frustum"), Name("queue"), "cpu");
	document = WithSetting(document, Name("delta-upload"), Name("queue"), "transfer");
	document = WithSetting(document, Name("gbuffer"), Name("queue"), "graphics");
	document = WithSetting(document, Name("ssao"), Name("async"), "allow");

	RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("queued#1"), graph));
}

TEST_CASE("entity culling nodes compose and may repeat in an authored graph", "[render][graph][cull]") {
	RenderGraph graph;
	const auto resource = [&graph](const char *name, engine::graph::ResourceKind kind) {
		return graph.AddResource({.Name = Name(name), .Kind = kind});
	};
	const engine::graph::ResourceId camera = resource("camera", engine::graph::ResourceKind::Camera);
	const engine::graph::ResourceId all = resource("all", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId tagged = resource("tagged", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId retagged = resource("retagged", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId near = resource("near", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId ordered = resource("ordered", engine::graph::ResourceKind::Entities);

	const auto node = [&graph](
						  const char *name,
						  const char *kind,
						  std::vector<engine::graph::ResourceId> reads,
						  std::vector<engine::graph::ResourceId> writes,
						  std::vector<engine::graph::NodeParameter> parameters = {}
					  ) {
		engine::graph::Node value;
		value.Name = Name(name);
		value.Kind = Name(kind);
		value.Reads = std::move(reads);
		value.Writes = std::move(writes);
		value.Parameters = std::move(parameters);
		REQUIRE(graph.AddNode(value).IsValid());
	};
	node("camera", "camera", {}, {camera});
	node("entities", "entities", {}, {all});
	node("characters", "filter-tag", {all}, {tagged}, {{Name("mask"), "0x3"}});
	node("players", "filter-tag", {tagged}, {retagged}, {{Name("mask"), "0x1"}});
	node("nearby", "cull-distance", {retagged, camera}, {near}, {{Name("radius"), "128.5"}});
	node("order", "order-draw", {near, camera}, {ordered});

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("filtered#1"), graph));
}

TEST_CASE("culling hints cannot be attached to a pass that ignores entity filters", "[render][graph][cull]") {
	Renderer renderer;
	const auto install = [&](Name pipeline, Name node, std::string value) {
		const engine::graph::PipelineDocument document =
			WithSetting(engine::graph::DefaultPbrDocument(), node, Name("culling"), std::move(value));
		RenderGraph graph;
		Name offender;
		REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
		return renderer.SetPipeline(pipeline, graph);
	};

	CHECK_FALSE(install(Name("ignored-cull#1"), Name("gbuffer"), "none"));
	CHECK(install(Name("unculled#1"), Name("cull-frustum"), "none"));

	// Accepted since the backend grew its depth pyramid and indirect draw
	// path: the default document carries the gbuffer pass whose early phase
	// seeds the pyramid.
	CHECK(install(Name("occlusion#1"), Name("cull-frustum"), "occlusion"));
}

TEST_CASE("occlusion culling is refused without the pass that seeds its pyramid", "[render][graph][cull]") {
	RenderGraph graph;
	const auto resource = [&graph](const char *name, engine::graph::ResourceKind kind) {
		return graph.AddResource({.Name = Name(name), .Kind = kind});
	};
	const engine::graph::ResourceId camera = resource("camera", engine::graph::ResourceKind::Camera);
	const engine::graph::ResourceId all = resource("all", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId visible = resource("visible", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId ordered = resource("ordered", engine::graph::ResourceKind::Entities);

	const auto node = [&graph](
						  const char *name,
						  const char *kind,
						  std::vector<engine::graph::ResourceId> reads,
						  std::vector<engine::graph::ResourceId> writes,
						  std::vector<engine::graph::NodeParameter> parameters = {}
					  ) {
		engine::graph::Node value;
		value.Name = Name(name);
		value.Kind = Name(kind);
		value.Reads = std::move(reads);
		value.Writes = std::move(writes);
		value.Parameters = std::move(parameters);
		REQUIRE(graph.AddNode(value).IsValid());
	};
	node("camera", "camera", {}, {camera});
	node("entities", "entities", {}, {all});
	node("cull", "cull-frustum", {all, camera}, {visible}, {{Name("culling"), "occlusion"}});
	node("order", "order-draw", {visible, camera}, {ordered});

	// No gbuffer pass, so nothing writes the depth the pyramid reduces - the
	// hint authored a cull nothing can feed.
	Renderer renderer;
	CHECK_FALSE(renderer.SetPipeline(Name("blind-occlusion#1"), graph));
}

TEST_CASE("a frame result reports authored node names", "[render]") {
	FrameResult result;
	CHECK_FALSE(result.Ran(Name("geometry")));

	result.Nodes = {Name("geometry"), Name("ao.quality"), Name("present")};
	CHECK(result.Ran(Name("geometry")));
	CHECK(result.Ran(Name("ao.quality")));
	CHECK(result.Ran(Name("present")));
	CHECK_FALSE(result.Ran(Name("shadow")));
}

TEST_CASE("a batched frame result adds counters and de-duplicates nodes", "[render][batch]") {
	FrameResult frame;
	frame.DrawCalls = 2;
	frame.Triangles = 12;
	frame.Nodes = {Name("shadow"), Name("gbuffer")};

	FrameResult view;
	view.Presented = true;
	view.DrawCalls = 3;
	view.Triangles = 24;
	view.SurfaceInstances = 2;
	view.SurfacePasses = 1;
	view.PortalPasses = 4;
	view.RibbonVertices = 22;
	view.Particles = 8;
	view.Culled = 7;
	view.ScheduledReadBytes = 1024;
	view.ScheduledWriteBytes = 2048;
	view.QueueTransferBytes = 512;
	view.UploadedBytes = 4096;
	view.UploadCommandBuffers = 3;
	view.ComputeDispatches = 4;
	view.AsyncComputeCommandBuffers = 2;
	view.ConcurrentWaves = 2;
	view.Nodes = {Name("gbuffer"), Name("tonemap"), Name("present")};

	frame.Accumulate(view);

	CHECK(frame.Presented);
	CHECK(frame.DrawCalls == 5);
	CHECK(frame.Triangles == 36);
	CHECK(frame.SurfaceInstances == 2);
	CHECK(frame.SurfacePasses == 1);
	CHECK(frame.PortalPasses == 4);
	CHECK(frame.RibbonVertices == 22);
	CHECK(frame.Particles == 8);
	CHECK(frame.Culled == 7);
	CHECK(frame.ScheduledReadBytes == 1024);
	CHECK(frame.ScheduledWriteBytes == 2048);
	CHECK(frame.QueueTransferBytes == 512);
	CHECK(frame.UploadedBytes == 4096);
	CHECK(frame.UploadCommandBuffers == 3);
	CHECK(frame.ComputeDispatches == 4);
	CHECK(frame.AsyncComputeCommandBuffers == 2);
	CHECK(frame.ConcurrentWaves == 2);
	CHECK(
		frame.Nodes == std::vector<Name>{Name("shadow"), Name("gbuffer"), Name("tonemap"), Name("present")}
	);
}

TEST_CASE("a renderer returns the complete lighting state it was given", "[render][batch]") {
	Renderer renderer;
	engine::scene::WorldLighting lighting;
	lighting.Direction = {0.0f, -2.0f, 0.0f};
	lighting.Ambient = {0.1f, 0.2f, 0.3f};
	lighting.OutdoorAmbient = {0.4f, 0.5f, 0.6f};
	lighting.Direct = {0.7f, 0.8f, 0.9f};
	lighting.FogColor = {0.11f, 0.22f, 0.33f};
	lighting.FogStart = 12.0f;
	lighting.FogEnd = 48.0f;
	lighting.BloomThreshold = 0.75f;
	lighting.BloomIntensity = 1.5f;
	lighting.BloomRadius = 11.0f;
	lighting.DepthOfFieldIntensity = 0.65f;
	lighting.DepthOfFieldFocusDistance = 32.0f;
	lighting.DepthOfFieldFocusRange = 6.0f;
	lighting.DepthOfFieldRadius = 8.0f;
	lighting.GodRayIntensity = 1.75f;
	lighting.GodRayThreshold = 0.4f;
	lighting.GodRayRadius = 64.0f;
	lighting.EnvironmentState.Skybox = engine::scene::SkyboxSource::Compute;
	lighting.EnvironmentState.SkyCompute.Seed = 83;
	renderer.SetLighting(lighting);

	const engine::scene::WorldLighting current = renderer.CurrentLighting();
	CHECK(current.Direction == (engine::core::Vector3{0.0f, -1.0f, 0.0f}));
	CHECK(current.Ambient == lighting.Ambient);
	CHECK(current.OutdoorAmbient == lighting.OutdoorAmbient);
	CHECK(current.Direct == lighting.Direct);
	CHECK(current.FogColor == lighting.FogColor);
	CHECK(current.FogStart == 12.0f);
	CHECK(current.FogEnd == 48.0f);
	CHECK(current.BloomThreshold == 0.75f);
	CHECK(current.BloomIntensity == 1.5f);
	CHECK(current.BloomRadius == 11.0f);
	CHECK(current.DepthOfFieldIntensity == 0.65f);
	CHECK(current.DepthOfFieldFocusDistance == 32.0f);
	CHECK(current.DepthOfFieldFocusRange == 6.0f);
	CHECK(current.DepthOfFieldRadius == 8.0f);
	CHECK(current.GodRayIntensity == 1.75f);
	CHECK(current.GodRayThreshold == 0.4f);
	CHECK(current.GodRayRadius == 64.0f);
	CHECK(current.EnvironmentState.Skybox == engine::scene::SkyboxSource::Compute);
	CHECK(current.EnvironmentState.SkyCompute.Seed == 83);
}

// The other half of D00016's neighbourhood: a decision that was recorded and
// not enforced.
//
// v0.7 decided that a studio with several viewports draws them one after
// another - the passes share one command buffer and one device, so parallel
// recording would serialise at submit and would cost the ordering that makes a
// docked viewport show *this* frame. That went into `ROADMAP.md` and nothing
// checked it, which is the shape of every stale claim this repository has found.
//
// **No device is created here.** The owner is claimed by the constructor and
// re-claimed by `Initialise`, precisely so the contract can be exercised on a
// build machine with no GPU. What cannot be asserted is the abort itself -
// `RequireOwningThread` calls `std::abort`, on purpose, and a test that survived
// it would be testing something else.
TEST_CASE("a renderer is owned by one thread", "[render]") {
	engine::render::Renderer renderer;

	CHECK(renderer.IsOnOwningThread());

	bool ownedElsewhere = true;
	std::thread other([&renderer, &ownedElsewhere] { ownedElsewhere = renderer.IsOnOwningThread(); });
	other.join();

	// The assertion the contract is made of: a second thread is not the owner,
	// so `Render` from a worker is refused rather than racing the frame the main
	// thread is recording.
	CHECK_FALSE(ownedElsewhere);

	// And the owner is unchanged by having been asked from elsewhere - the check
	// is a comparison and never a claim.
	CHECK(renderer.IsOnOwningThread());
}

TEST_CASE("pack-channels admits native scalar lanes and rejects impossible selectors", "[render][graph]") {
	using engine::graph::Node;
	using engine::graph::NodeParameter;
	using engine::graph::NodeScope;
	using engine::graph::ResourceFormat;
	using engine::graph::ResourceKind;

	const auto make = [](ResourceFormat greenFormat, std::string greenComponent, bool consumePacked = false) {
		RenderGraph graph;
		const auto depth = graph.AddResource(
			{.Name = Name("depth"),
			 .Kind = ResourceKind::Colour,
			 .Format = ResourceFormat::R32F,
			 .External = true}
		);
		const auto green = graph.AddResource(
			{.Name = Name("green"), .Kind = ResourceKind::Colour, .Format = greenFormat, .External = true}
		);
		const auto material = graph.AddResource(
			{.Name = Name("material"),
			 .Kind = ResourceKind::Colour,
			 .Format = ResourceFormat::RGBA8,
			 .External = true}
		);
		const auto packed = graph.AddResource(
			{.Name = Name("packed"), .Kind = ResourceKind::Colour, .Format = ResourceFormat::RGBA32F}
		);
		graph.AddNode({
			.Name = Name("pack"),
			.Kind = Name("pack-channels"),
			.Reads = {depth, green, material, material},
			.ReadPorts = {Name("r"), Name("g"), Name("b"), Name("a")},
			.Writes = {packed},
			.WritePorts = {Name("packed")},
			.Scope = NodeScope::View,
			.Parameters = {
				{.Key = Name("r-component"), .Value = "0"},
				{.Key = Name("g-component"), .Value = std::move(greenComponent)},
				{.Key = Name("b-component"), .Value = "0"},
				{.Key = Name("a-component"), .Value = "2"},
			},
		});
		if (consumePacked) {
			const auto sampled = graph.AddResource(
				{.Name = Name("sampled"), .Kind = ResourceKind::Colour, .Format = ResourceFormat::RGBA16F}
			);
			graph.AddNode({
				.Name = Name("sample-packed"),
				.Kind = Name("blit"),
				.Reads = {packed},
				.ReadPorts = {Name("source")},
				.Writes = {sampled},
				.WritePorts = {Name("colour")},
				.Scope = NodeScope::View,
			});
		}
		return graph;
	};

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("pack-valid"), make(ResourceFormat::R8, "0", true)));
	CHECK_FALSE(renderer.SetPipeline(Name("pack-r8-component"), make(ResourceFormat::R8, "1")));
	CHECK_FALSE(renderer.SetPipeline(Name("pack-uint"), make(ResourceFormat::R32U, "0")));
}
