#include "PipelineCompiler.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string_view>

TEST_SUITE_ID("engine.render.pipelineparity")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.graph.schedule")

namespace {
	using engine::core::Name;
	using namespace engine::graph;
	using engine::render::PipelineAdmissionStage;

	enum class Fixture {
		ValidBlit,
		ValidDispatch,
		InvalidBlitFormat,
		ReadsBeforeWrite,
		InvalidHint,
		MissingBackend,
		MissingShadowDependency
	};

	void Resource(
		PipelineDocument &document,
		std::string_view name,
		ResourceKind kind = ResourceKind::Colour,
		ResourceFormat format = ResourceFormat::RGBA16F,
		bool external = false
	) {
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = Name(name),
			 .Resource = kind,
			 .Format = format,
			 .External = external}
		);
	}

	void Node(
		PipelineDocument &document,
		std::string_view name,
		std::string_view kind,
		NodeScope scope = NodeScope::View
	) {
		document.Record(
			{.Kind = EditKind::AddNode, .Name = Name(name), .NodeKind = Name(kind), .Scope = scope}
		);
	}

	void Edge(PipelineDocument &document, EditKind kind, std::string_view resource, std::string_view port) {
		document.Record({.Kind = kind, .Target = Name(resource), .Key = Name(port)});
	}

	PipelineDocument Document(Fixture fixture) {
		PipelineDocument document;
		if (fixture == Fixture::ValidDispatch) {
			Resource(document, "result", ResourceKind::Storage);
			Node(document, "compute", "dispatch");
			Edge(document, EditKind::Writes, "result", "result");
		} else if (fixture == Fixture::InvalidBlitFormat) {
			Resource(document, "source", ResourceKind::Colour, ResourceFormat::RGBA16F, true);
			Resource(document, "output");
			Node(document, "copy", "blit");
			document.Record({.Kind = EditKind::Set, .Key = Name("format"), .Value = "unknown"});
			Edge(document, EditKind::Reads, "source", "source");
			Edge(document, EditKind::Writes, "output", "colour");
		} else if (fixture == Fixture::ReadsBeforeWrite) {
			Resource(document, "input");
			Resource(document, "output");
			Node(document, "copy", "blit");
			Edge(document, EditKind::Reads, "input", "source");
			Edge(document, EditKind::Writes, "output", "colour");
		} else if (fixture == Fixture::InvalidHint) {
			Resource(document, "storage", ResourceKind::Storage);
			Node(document, "bad-dispatch", "dispatch");
			document.Record({.Kind = EditKind::Set, .Key = Name("dispatch.x"), .Value = "zero"});
			Edge(document, EditKind::Writes, "storage", "result");
		} else if (fixture == Fixture::MissingBackend) {
			Resource(document, "output");
			Node(document, "future", "future-compositor");
			Edge(document, EditKind::Writes, "output", "colour");
		} else if (fixture == Fixture::MissingShadowDependency) {
			Resource(document, "depth", ResourceKind::Depth, ResourceFormat::D32F);
			Node(document, "orphan-shadow", "shadow", NodeScope::World);
			Edge(document, EditKind::Writes, "depth", "depth");
		} else {
			Resource(document, "source", ResourceKind::Colour, ResourceFormat::RGBA16F, true);
			Resource(document, "output");
			Node(document, "copy", "blit");
			Edge(document, EditKind::Reads, "source", "source");
			Edge(document, EditKind::Writes, "output", "colour");
		}
		return document;
	}

	struct Case {
		std::string_view Label;
		Fixture Input;
		PipelineDocumentStatus BuildStatus;
		GraphStatus GraphResult;
		ScheduleStatus ScheduleResult;
		std::optional<PipelineAdmissionStage> RendererStage;
		std::string_view Offender;
		std::string_view BuildOffender;
		std::string_view ScheduleOffender;
		std::string_view Reason;
	};
}

TEST_CASE(
	"capability admission preserves graph planning and names the authored node",
	"[render][graph][capabilities]"
) {
	RegisterRenderNodeKinds();
	enum class Gap { None, Compute, StorageTextures, Format };
	struct Row {
		std::string_view Label;
		Fixture Input;
		Gap Missing;
		std::optional<PipelineAdmissionStage> Stage;
		std::string_view Offender;
		std::string_view Reason;
	};
	const std::array rows{
		Row{"dispatch accepted", Fixture::ValidDispatch, Gap::None, {}, {}, {}},
		Row{"dispatch needs compute",
			Fixture::ValidDispatch,
			Gap::Compute,
			PipelineAdmissionStage::Capability,
			"compute",
			"the device has no compute pipeline support"},
		Row{"dispatch needs storage textures",
			Fixture::ValidDispatch,
			Gap::StorageTextures,
			PipelineAdmissionStage::Capability,
			"compute",
			"the device cannot write storage textures"},
		Row{"dispatch needs RGBA16F",
			Fixture::ValidDispatch,
			Gap::Format,
			PipelineAdmissionStage::Capability,
			"compute",
			"the device does not support a required texture format: RGBA16F"},
		Row{"blit accepted", Fixture::ValidBlit, Gap::None, {}, {}, {}},
		Row{"blit needs target format",
			Fixture::ValidBlit,
			Gap::Format,
			PipelineAdmissionStage::Capability,
			"copy",
			"the device does not support a required texture format: RGBA16F"},
		Row{"validation precedes capability",
			Fixture::InvalidBlitFormat,
			Gap::Format,
			PipelineAdmissionStage::Validation,
			"copy",
			"blit target format is not recognised"},
	};

	for (const Row &row : rows) {
		DYNAMIC_SECTION(row.Label) {
			RenderGraph graph;
			Name offender;
			CHECK(Build(Document(row.Input), graph, offender) == PipelineDocumentStatus::Ok);
			CompiledGraph compiled;
			CHECK(graph.Compile(compiled, offender) == GraphStatus::Ok);
			ExecutionSchedule schedule;
			CHECK(CompileSchedule(graph, schedule, offender) == ScheduleStatus::Ok);

			engine::render::DeviceCaps caps;
			caps.HasCompute = row.Missing != Gap::Compute;
			caps.HasStorageTextures = row.Missing != Gap::StorageTextures;
			if (row.Missing != Gap::Format) caps.Formats.push_back(ResourceFormat::RGBA16F);
			const auto admission =
				engine::render::CompilePipeline(Name("capability-parity"), graph, &caps, {});
			if (!row.Stage) {
				CHECK(bool(admission));
				continue;
			}
			CHECK_FALSE(bool(admission));
			CHECK(admission.Failure.Stage == *row.Stage);
			CHECK(admission.Failure.Offender == Name(row.Offender));
			CHECK(admission.Failure.Reason == row.Reason);
		}
	}
}

TEST_CASE(
	"graph Build and Schedule diagnostics match renderer admission where they share policy", "[render][graph]"
) {
	RegisterRenderNodeKinds();
	const std::array cases{
		Case{
			"valid blit",
			Fixture::ValidBlit,
			PipelineDocumentStatus::Ok,
			GraphStatus::Ok,
			ScheduleStatus::Ok,
			{},
			{},
			{},
			{},
			{}
		},
		Case{
			"read before write",
			Fixture::ReadsBeforeWrite,
			PipelineDocumentStatus::Invalid,
			GraphStatus::ReadsBeforeWrite,
			ScheduleStatus::MissingProducer,
			PipelineAdmissionStage::Graph,
			"copy",
			"input",
			"input",
			"a node reads something nothing earlier wrote"
		},
		Case{
			"invalid dispatch hint",
			Fixture::InvalidHint,
			PipelineDocumentStatus::Invalid,
			GraphStatus::Ok,
			ScheduleStatus::InvalidHint,
			PipelineAdmissionStage::Schedule,
			"bad-dispatch",
			"bad-dispatch",
			"bad-dispatch",
			"a scheduling hint is not valid"
		},
		// Build and graph scheduling cannot judge installed backend support.
		Case{
			"uninstalled kind",
			Fixture::MissingBackend,
			PipelineDocumentStatus::Ok,
			GraphStatus::Ok,
			ScheduleStatus::Ok,
			PipelineAdmissionStage::Backend,
			"future-compositor",
			{},
			{},
			"the renderer has no backend node for this kind"
		},
		// The device-facing compiler enforces the mesh residency dependency.
		Case{
			"shadow without mesh residency",
			Fixture::MissingShadowDependency,
			PipelineDocumentStatus::Ok,
			GraphStatus::Ok,
			ScheduleStatus::Ok,
			PipelineAdmissionStage::Validation,
			"orphan-shadow",
			{},
			{},
			"shadow work has no graph dependency on mesh-residency"
		},
	};

	for (const Case &row : cases) {
		DYNAMIC_SECTION(row.Label) {
			RenderGraph graph;
			Name buildOffender;
			CHECK(Build(Document(row.Input), graph, buildOffender) == row.BuildStatus);
			if (row.BuildStatus == PipelineDocumentStatus::Invalid)
				CHECK(buildOffender == Name(row.BuildOffender));

			CompiledGraph compiled;
			Name graphOffender;
			CHECK(graph.Compile(compiled, graphOffender) == row.GraphResult);
			if (row.GraphResult != GraphStatus::Ok) CHECK(graphOffender == Name(row.Offender));

			ExecutionSchedule schedule;
			Name scheduleOffender;
			CHECK(CompileSchedule(graph, schedule, scheduleOffender) == row.ScheduleResult);
			if (row.ScheduleResult != ScheduleStatus::Ok)
				CHECK(scheduleOffender == Name(row.ScheduleOffender));

			const auto admission = engine::render::CompilePipeline(Name("parity"), graph, nullptr, {});
			if (!row.RendererStage) {
				CHECK(bool(admission));
				continue;
			}
			CHECK_FALSE(bool(admission));
			CHECK(admission.Failure.Stage == *row.RendererStage);
			CHECK(admission.Failure.Offender == Name(row.Offender));
			CHECK(admission.Failure.Reason == row.Reason);
			if (*row.RendererStage == PipelineAdmissionStage::Graph)
				CHECK(admission.Failure.Reason == Describe(row.GraphResult));
			if (*row.RendererStage == PipelineAdmissionStage::Schedule)
				CHECK(admission.Failure.Reason == Describe(row.ScheduleResult));
		}
	}
}
