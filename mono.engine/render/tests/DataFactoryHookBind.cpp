#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace engine::render {
	namespace {
		RenderHookSpec Spec(std::string_view name, DataCaptureChannel channel) {
			return {
				.Name = core::Name(name),
				.Kind = RenderHookKind::DataCapture,
				.NodeKind = core::Name("capture"),
				.SchemaVersion = 1,
				.Required = true,
				.Channels = {channel},
				.ChannelCount = 1,
			};
		}
		HookConnectionRequest Connection(Renderer &renderer) {
			graph::RenderGraph graph;
			core::Name offender;
			REQUIRE(
				graph::Build(graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
				graph::PipelineDocumentStatus::Ok
			);
			const core::Name name("hook-test-pipeline");
			REQUIRE(renderer.SetPipeline(name, graph));
			const auto pipeline = renderer.DescribePipeline(name, 1, 1);
			return {
				.Session = {.WorldName = "test-world"},
				.Pipeline = name,
				.PipelineRevision = pipeline->Revision,
				.ViewSlot = 0,
				.CaptureNode = core::Name("data-capture"),
				.ViewWidth = 1,
				.ViewHeight = 1
			};
		}
		DataCaptureRequest Request() {
			return {
				.SnapshotId = "snapshot",
				.Pipeline = core::Name("hook-test-pipeline"),
				.CaptureNode = core::Name("data-capture"),
				.Channels = {DataCaptureChannel::RgbLinearHdr},
				.ObjectLabels = {},
				.SemanticLabels = {},
				.PartLabels = {}
			};
		}
	}

	TEST_CASE("DataFactoryHookBind registers and connects atomically", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookHandle colour =
			bind.RegisterHook(Spec("data_capture.test_colour", DataCaptureChannel::RgbLinearHdr));
		CHECK(colour.IsValid());
		CHECK(
			bind.RegisterHook(Spec("data_capture.test_colour", DataCaptureChannel::RgbLinearHdr)).Slot ==
			colour.Slot
		);
		CHECK(bind.FindHook(core::Name("data_capture.test_colour")).Slot == colour.Slot);
		CHECK_FALSE(bind.FindHook(core::Name("data_capture.missing")).IsValid());
		CHECK_FALSE(
			bind.RegisterHook(Spec("data_capture.test_colour", DataCaptureChannel::LinearDepth)).IsValid()
		);
		REQUIRE(bind.DescribeHooks().size() == 1);
		CHECK(bind.DescribeHooks().front().Name == core::Name("data_capture.test_colour"));

		const std::array invalid{colour, HookHandle{}};
		CHECK(bind.ConnectHooks(Connection(renderer), invalid).Status == HookBindStatus::UnknownHandle);
		const std::array hooks{colour};
		const ConnectHooksResult connected = bind.ConnectHooks(Connection(renderer), hooks);
		REQUIRE(connected.Status == HookBindStatus::Ok);
		CHECK(connected.Connection.IsValid());
		CHECK(
			bind.ConnectHooks(Connection(renderer), std::array{colour, colour}).Status ==
			HookBindStatus::Conflict
		);
		const HookHandle duplicate =
			bind.RegisterHook(Spec("data_capture.duplicate_colour", DataCaptureChannel::RgbLinearHdr));
		CHECK(
			bind.ConnectHooks(Connection(renderer), std::array{colour, duplicate}).Status ==
			HookBindStatus::Conflict
		);
	}

	TEST_CASE("DataFactoryHookBind invalidates batches on disconnect", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookHandle colour =
			bind.RegisterHook(Spec("data_capture.batch_colour", DataCaptureChannel::RgbLinearHdr));
		const ConnectHooksResult connected = bind.ConnectHooks(Connection(renderer), std::array{colour});
		REQUIRE(connected.Status == HookBindStatus::Ok);
		DataCaptureRequest request = Request();
		const auto batch = bind.ArmDataCapture(connected.Connection, request);
		REQUIRE(batch);
		bind.DisconnectHooks(std::array{connected.Connection});
		CHECK(bind.CallHooks(connected.Connection, *batch, {}).Status == HookBindStatus::Stale);
		CHECK_FALSE(bind.TakeCompleted(*batch));
	}

	TEST_CASE("DataFactoryHookBind bounds batches and rejects stale generations", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookHandle colour =
			bind.RegisterHook(Spec("data_capture.capacity_colour", DataCaptureChannel::RgbLinearHdr));
		const ConnectHooksResult connected = bind.ConnectHooks(Connection(renderer), std::array{colour});
		REQUIRE(connected.Status == HookBindStatus::Ok);
		const DataCaptureRequest request = Request();
		std::array<BatchHandle, MAX_DATA_FACTORY_BATCHES> batches{};
		for (BatchHandle &batch : batches) {
			const auto armed = bind.ArmDataCapture(connected.Connection, request);
			REQUIRE(armed);
			batch = *armed;
		}
		CHECK_FALSE(bind.ArmDataCapture(connected.Connection, request));
		bind.Cancel(batches.front());
		const auto reused = bind.ArmDataCapture(connected.Connection, request);
		REQUIRE(reused);
		CHECK(reused->Slot == batches.front().Slot);
		CHECK(reused->Generation != batches.front().Generation);
		CHECK(bind.CallHooks(connected.Connection, batches.front(), {}).Status == HookBindStatus::Stale);
	}

	TEST_CASE("DataFactoryHookBind publishes shutdown as terminal", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookHandle colour =
			bind.RegisterHook(Spec("data_capture.shutdown_colour", DataCaptureChannel::RgbLinearHdr));
		const ConnectHooksResult connected = bind.ConnectHooks(Connection(renderer), std::array{colour});
		REQUIRE(connected.Status == HookBindStatus::Ok);
		const auto batch = bind.ArmDataCapture(
			connected.Connection,
			{.SnapshotId = "snapshot",
			 .Pipeline = core::Name("hook-test-pipeline"),
			 .CaptureNode = core::Name("data-capture"),
			 .Channels = {DataCaptureChannel::RgbLinearHdr},
			 .ObjectLabels = {},
			 .SemanticLabels = {},
			 .PartLabels = {}}
		);
		REQUIRE(batch);
		bind.Shutdown();
		CHECK(bind.CallHooks(connected.Connection, *batch, {}).Status == HookBindStatus::Invalid);
		const auto completed = bind.TakeCompleted(*batch);
		REQUIRE(completed);
		CHECK(completed->Capture.Status == DataCaptureStatus::Cancelled);
		REQUIRE(completed->Capture.Planes.size() == 1);
		CHECK(completed->Capture.Planes.front().Status == DataCaptureStatus::Cancelled);
		CHECK(completed->Capture.Planes.front().Channel == DataCaptureChannel::RgbLinearHdr);
		CHECK(completed->Capture.Planes.front().CaptureNode == core::Name("data-capture"));
		CHECK_FALSE(bind.TakeCompleted(*batch));
	}

	TEST_CASE("DataFactoryHookBind refuses an armed request that changes connection identity", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookHandle colour =
			bind.RegisterHook(Spec("data_capture.identity_colour", DataCaptureChannel::RgbLinearHdr));
		const ConnectHooksResult connected = bind.ConnectHooks(Connection(renderer), std::array{colour});
		REQUIRE(connected.Status == HookBindStatus::Ok);
		DataCaptureRequest request = Request();
		request.Pipeline = core::Name("wrong-pipeline");
		CHECK_FALSE(bind.ArmDataCapture(connected.Connection, request));
		request.Pipeline = core::Name("hook-test-pipeline");
		request.CaptureNode = core::Name("wrong-node");
		CHECK_FALSE(bind.ArmDataCapture(connected.Connection, request));
		request.CaptureNode = core::Name("data-capture");
		request.ViewSlot = 1;
		CHECK_FALSE(bind.ArmDataCapture(connected.Connection, request));
	}

	TEST_CASE("DataFactoryHookBind rejects a stale revision before submission", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookHandle colour =
			bind.RegisterHook(Spec("data_capture.stale_colour", DataCaptureChannel::RgbLinearHdr));
		const HookConnectionRequest connectionRequest = Connection(renderer);
		const ConnectHooksResult connected = bind.ConnectHooks(connectionRequest, std::array{colour});
		REQUIRE(connected.Status == HookBindStatus::Ok);
		const auto batch = bind.ArmDataCapture(connected.Connection, Request());
		REQUIRE(batch);
		RenderObservationContext context;
		context.WorldName = core::Name("test-world");
		context.SnapshotId = "snapshot";
		context.Pipeline = core::Name("hook-test-pipeline");
		context.PipelineRevision = connectionRequest.PipelineRevision + 1;
		context.ViewSlot = 0;
		CHECK(bind.CallHooks(connected.Connection, *batch, context).Status == HookBindStatus::Invalid);
	}
}
