#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <optional>

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
				.PartLabels = {},
				.LocalLightIds = {}
			};
		}
		RenderHookSpec CameraSpec() {
			return {
				.Name = core::Name("view.camera"),
				.Kind = RenderHookKind::ViewMutation,
				.Access = RenderHookAccess::SynchronousMutation,
				.NodeKind = core::Name("view"),
				.SchemaVersion = 1,
				.Required = true,
				.Channels = {},
				.ChannelCount = 0,
				.MutatedFields =
					{
						RenderHookMutatedField::CameraFrame,
						RenderHookMutatedField::Camera,
						RenderHookMutatedField::Projection,
					},
				.MutatedFieldCount = 3,
			};
		}
		ViewMutationIdentity MutationIdentity(const HookConnectionRequest &connection) {
			return {
				.WorldName = connection.Session.WorldName,
				.SnapshotId = "snapshot",
				.Pipeline = connection.Pipeline,
				.PipelineRevision = connection.PipelineRevision,
				.ViewSlot = connection.ViewSlot,
			};
		}
		View ViewFor(const ViewMutationIdentity &identity) {
			View view;
			view.WorldName = core::Name(identity.WorldName);
			view.SnapshotId = identity.SnapshotId;
			view.Pipeline = identity.Pipeline;
			view.Slot = identity.ViewSlot;
			view.CameraFrame = core::CFrame(core::Vector3{1.0f, 2.0f, 3.0f});
			return view;
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
				 .PartLabels = {},
				 .LocalLightIds = {}}
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

	TEST_CASE("DataFactoryHookBind rejects a replaced pipeline revision before submission", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookHandle colour =
			bind.RegisterHook(Spec("data_capture.stale_colour", DataCaptureChannel::RgbLinearHdr));
		const HookConnectionRequest connectionRequest = Connection(renderer);
		const ConnectHooksResult connected = bind.ConnectHooks(connectionRequest, std::array{colour});
		REQUIRE(connected.Status == HookBindStatus::Ok);
		const auto batch = bind.ArmDataCapture(connected.Connection, Request());
		REQUIRE(batch);
		graph::RenderGraph replacement;
		core::Name offender;
		REQUIRE(
			graph::Build(graph::DefaultPbrDataCaptureDocument(), replacement, offender) ==
			graph::PipelineDocumentStatus::Ok
		);
		REQUIRE(renderer.SetPipeline(core::Name("hook-test-pipeline"), replacement));
		RenderObservationContext context;
		context.WorldName = core::Name("test-world");
		context.SnapshotId = "snapshot";
		context.Pipeline = core::Name("hook-test-pipeline");
		context.PipelineRevision = connectionRequest.PipelineRevision;
		context.ViewSlot = 0;
		CHECK(bind.CallHooks(connected.Connection, *batch, context).Status == HookBindStatus::Invalid);
	}

	TEST_CASE("DataFactoryHookBind applies an owned camera patch to only the matching view", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookConnectionRequest connection = Connection(renderer);
		const HookHandle hook = bind.RegisterHook(CameraSpec());
		REQUIRE(hook.IsValid());
		const ViewMutationIdentity identity = MutationIdentity(connection);
		const core::CFrame replacement(core::Vector3{9.0f, 8.0f, 7.0f});
		const scene::Camera camera{
			.FieldOfViewRadians = 0.9f, .NearPlane = 0.2f, .FarPlane = 200.0f, .RenderFeatures = {}
		};
		const ArmViewMutationResult armed = bind.ArmViewMutation(
			hook,
			{.Identity = identity,
			 .CameraFrame = replacement,
			 .Camera = camera,
			 .Projection = glm::mat4(1.0f)}
		);
		REQUIRE(armed.Status == HookBindStatus::Ok);
		View caller = ViewFor(identity);
		View prepared = caller;
		REQUIRE(bind.ConsumeViewMutation(identity, prepared));
		CHECK(caller.CameraFrame.Position == core::Vector3{1.0f, 2.0f, 3.0f});
		CHECK(prepared.CameraFrame.Position == replacement.Position);
		CHECK(prepared.Camera.FieldOfViewRadians == camera.FieldOfViewRadians);
		CHECK(prepared.Projection == glm::mat4(1.0f));
		CHECK(prepared.Damage.Scene);
		CHECK(prepared.Damage.Viewport);
		CHECK_FALSE(bind.HasViewMutation(identity));
	}

	TEST_CASE("Renderer discovers view.camera as a synchronous mutation hook", "[render]") {
		Renderer renderer;
		const auto hooks = renderer.Hooks().DescribeHooks();
		const auto found = std::find_if(hooks.begin(), hooks.end(), [](const RenderHookCapability &hook) {
			return hook.Name == core::Name("view.camera");
		});
		REQUIRE(found != hooks.end());
		CHECK(found->Kind == RenderHookKind::ViewMutation);
		CHECK(found->Access == RenderHookAccess::SynchronousMutation);
		CHECK(found->ChannelCount == 0);
		REQUIRE(found->MutatedFieldCount == 3);
		CHECK(found->MutatedFields[0] == RenderHookMutatedField::CameraFrame);
		CHECK(found->MutatedFields[1] == RenderHookMutatedField::Camera);
		CHECK(found->MutatedFields[2] == RenderHookMutatedField::Projection);
	}

	TEST_CASE("DataFactoryHookBind validates and isolates camera patches atomically", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookConnectionRequest connection = Connection(renderer);
		const HookHandle hook = bind.RegisterHook(CameraSpec());
		const ViewMutationIdentity identity = MutationIdentity(connection);
		ViewCameraPatch invalid{
			.Identity = identity,
			.CameraFrame = core::CFrame{},
			.Camera = std::nullopt,
			.Projection = std::nullopt
		};
		invalid.CameraFrame->QuaternionW = std::numeric_limits<float>::quiet_NaN();
		CHECK(bind.ArmViewMutation(hook, invalid).Status == HookBindStatus::Invalid);
		const ArmViewMutationResult first = bind.ArmViewMutation(
			hook,
			{.Identity = identity,
			 .CameraFrame = std::nullopt,
			 .Camera = scene::Camera{},
			 .Projection = std::nullopt}
		);
		REQUIRE(first.Status == HookBindStatus::Ok);
		CHECK(
			bind.ArmViewMutation(
					hook,
					{.Identity = identity,
					 .CameraFrame = std::nullopt,
					 .Camera = scene::Camera{},
					 .Projection = std::nullopt}
			)
				.Status == HookBindStatus::Conflict
		);
		ViewMutationIdentity otherSnapshot = identity;
		otherSnapshot.SnapshotId = "other";
		CHECK(
			bind.ArmViewMutation(
					hook,
					{.Identity = otherSnapshot,
					 .CameraFrame = std::nullopt,
					 .Camera = scene::Camera{},
					 .Projection = std::nullopt}
			)
				.Status == HookBindStatus::Ok
		);
		ViewMutationIdentity otherView = identity;
		otherView.ViewSlot = 1;
		CHECK(
			bind.ArmViewMutation(
					hook,
					{.Identity = otherView,
					 .CameraFrame = std::nullopt,
					 .Camera = scene::Camera{},
					 .Projection = std::nullopt}
			)
				.Status == HookBindStatus::Ok
		);
		bind.Cancel(first.Mutation);
		CHECK_FALSE(bind.HasViewMutation(identity));
		CHECK(bind.PollViewMutation(first.Mutation).Status == ViewMutationStatus::Cancelled);
		bind.ReleaseViewMutation(first.Mutation);
		const ArmViewMutationResult reused = bind.ArmViewMutation(
			hook,
			{.Identity = identity,
			 .CameraFrame = std::nullopt,
			 .Camera = scene::Camera{},
			 .Projection = std::nullopt}
		);
		REQUIRE(reused.Status == HookBindStatus::Ok);
		CHECK(reused.Mutation.Generation != first.Mutation.Generation);
	}

	TEST_CASE("DataFactoryHookBind reclaims applied camera patches after pipeline replacement", "[render]") {
		Renderer renderer;
		DataFactoryHookBind bind(renderer);
		const HookConnectionRequest connection = Connection(renderer);
		const HookHandle hook = bind.RegisterHook(CameraSpec());
		const ViewMutationIdentity identity = MutationIdentity(connection);
		const ArmViewMutationResult armed = bind.ArmViewMutation(
			hook,
			{.Identity = identity,
			 .CameraFrame = std::nullopt,
			 .Camera = scene::Camera{},
			 .Projection = std::nullopt}
		);
		REQUIRE(armed.Status == HookBindStatus::Ok);
		View applied = ViewFor(identity);
		REQUIRE(bind.ConsumeViewMutation(identity, applied));
		CHECK(bind.PollViewMutation(armed.Mutation).Status == ViewMutationStatus::AppliedAwaitingRestore);
		graph::RenderGraph replacement;
		core::Name offender;
		REQUIRE(
			graph::Build(graph::DefaultPbrDataCaptureDocument(), replacement, offender) ==
			graph::PipelineDocumentStatus::Ok
		);
		REQUIRE(renderer.SetPipeline(connection.Pipeline, replacement));
		bind.Pump();
		CHECK(bind.PollViewMutation(armed.Mutation).Status == ViewMutationStatus::Stale);
		bind.ReleaseViewMutation(armed.Mutation);
		const auto current = renderer.ResolvePipelineIdentity(connection.Pipeline);
		REQUIRE(current);
		ViewMutationIdentity next = identity;
		next.PipelineRevision = current->Revision;
		CHECK(
			bind.ArmViewMutation(
					hook,
					{.Identity = next,
					 .CameraFrame = std::nullopt,
					 .Camera = scene::Camera{},
					 .Projection = std::nullopt}
			)
				.Status == HookBindStatus::Ok
		);
	}
}
