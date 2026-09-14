// The renderer owns the built-in hook registry. This stays device-free so it
// measures connection validation and no-device backpressure, not readback work.

#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <stdexcept>

TEST_SUITE_ID("engine.render.bench.data-capture-hooks")

namespace {
	constexpr size_t LIFECYCLES = 64;

	struct Fixture {
		engine::render::Renderer Renderer;
		engine::core::Name Pipeline{"bench.data-capture-hooks"};
		engine::render::HookConnectionRequest Connection;
		engine::render::DataCaptureRequest Request;
		engine::render::RenderObservationContext Context;
		engine::render::HookHandle Hook;

		Fixture() {
			engine::graph::RenderGraph graph;
			engine::core::Name offender;
			if (engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) !=
					engine::graph::PipelineDocumentStatus::Ok ||
				!Renderer.SetPipeline(Pipeline, graph))
				throw std::runtime_error("data capture hook benchmark graph setup failed");
			const auto described = Renderer.DescribePipeline(Pipeline, 1, 1);
			if (!described)
				throw std::runtime_error("data capture hook benchmark pipeline was not installed");
			Connection = {
				.Session = {.WorldName = "bench-world"},
				.Pipeline = Pipeline,
				.PipelineRevision = described->Revision,
				.ViewSlot = 0,
				.CaptureNode = engine::core::Name("data-capture"),
				.ViewWidth = 1,
				.ViewHeight = 1,
			};
			Request = {
				.SnapshotId = "bench-snapshot",
				.Pipeline = Pipeline,
				.CaptureNode = engine::core::Name("data-capture"),
				.Channels = {engine::render::DataCaptureChannel::RgbLinearHdr},
				.ObjectLabels = {},
				.SemanticLabels = {},
				.PartLabels = {},
			};
			Context = {
				.Pipeline = Pipeline,
				.Node = engine::render::DataCaptureNode(
					engine::core::Name("data-capture"), engine::render::DataCaptureChannel::RgbLinearHdr
				),
				.WorldName = engine::core::Name("bench-world"),
				.ViewSlot = 0,
				.SnapshotId = "bench-snapshot",
				.PipelineRevision = described->Revision,
				.Camera = {},
			};
			Hook = Renderer.Hooks().FindHook(engine::core::Name("data_capture.rgb_linear_hdr"));
			if (!Hook.IsValid())
				throw std::runtime_error("data capture hook benchmark hook was not registered");
		}
	};

	void RunLifecycles() {
		static Fixture fixture;
		for (size_t iteration = 0; iteration < LIFECYCLES; ++iteration) {
			const auto connected =
				fixture.Renderer.Hooks().ConnectHooks(fixture.Connection, std::array{fixture.Hook});
			if (connected.Status != engine::render::HookBindStatus::Ok)
				throw std::runtime_error("data capture hook benchmark connection failed");
			const auto batch = fixture.Renderer.Hooks().ArmDataCapture(connected.Connection, fixture.Request);
			if (!batch) throw std::runtime_error("data capture hook benchmark arm failed");
			const auto called =
				fixture.Renderer.Hooks().CallHooks(connected.Connection, *batch, fixture.Context);
			if (called.Status != engine::render::HookBindStatus::Backpressured)
				throw std::runtime_error("data capture hook benchmark expected no-device backpressure");
			fixture.Renderer.Hooks().Cancel(*batch);
			fixture.Renderer.Hooks().DisconnectHooks(std::array{connected.Connection});
		}
	}
}

BENCH("Data capture hooks | connect arm call cancel | 64 lifecycles", LIFECYCLES) {
	RunLifecycles();
}
