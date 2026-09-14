#include "CaptureRecordValidation.hpp"
#include "RenderFixture.hpp"
#include "SecondSurfaceDepth.hpp"

#include <engine/render/DataCapture.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.render.datacapture")

using namespace engine::render;

TEST_CASE("data capture channel names are stable", "[render][data-capture]") {
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::RgbLinearHdr)) == "rgb_linear_hdr");
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::LinearDepth)) == "linear_depth");
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::AmbientOcclusion)) == "ambient_occlusion"
	);
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::SemanticMask)) == "semantic_ids");
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::PartMask)) == "part_ids");
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::SecondSurfaceDepth)) ==
		"second_surface_depth"
	);
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::SecondSurfaceValidity)) ==
		"second_surface_validity"
	);
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::OpticalFlow)) == "optical_flow");
}

TEST_CASE("object label sidecars require bounded UTF-8 labels", "[render][data-capture]") {
	std::vector<DataCaptureObjectLabel> labels{{1, "alpha"}};
	CHECK(ValidDataCaptureObjectLabels(labels));
	labels[0].StableId = std::string{"\xC0\x80", 2};
	CHECK_FALSE(ValidDataCaptureObjectLabels(labels));

	labels.assign(MAX_DATA_CAPTURE_OBJECT_LABELS + 1, {});
	for (size_t index = 0; index < labels.size(); ++index) {
		labels[index].Label = static_cast<uint32_t>(index + 1);
		labels[index].StableId = "a";
	}
	CHECK_FALSE(ValidDataCaptureObjectLabels(labels));

	labels.assign(MAX_DATA_CAPTURE_OBJECT_LABEL_BYTES / 256 + 1, {});
	for (size_t index = 0; index < labels.size(); ++index) {
		labels[index].Label = static_cast<uint32_t>(index + 1);
		labels[index].StableId.assign(256, 'a');
	}
	CHECK_FALSE(ValidDataCaptureObjectLabels(labels));
}

TEST_CASE(
	"frame result keeps headless command submission separate from presentation", "[render][data-capture]"
) {
	FrameResult frame;
	FrameResult headless;
	headless.Submitted = true;
	frame.Accumulate(headless);
	CHECK(frame.Submitted);
	CHECK_FALSE(frame.Presented);
}

TEST_CASE("data capture camera convention records the engine projection", "[render][data-capture]") {
	const DataCaptureCameraConvention convention = DataCaptureCameraConventions();
	CHECK(convention.RightHandedWorld);
	CHECK(convention.CameraLooksNegativeZ);
	CHECK(convention.ClipYUp);
	CHECK(convention.DepthZeroToOne);
	CHECK(convention.ProjectionIsColumnMajor);
	CHECK(convention.MetresPerWorldUnit == 1.0f);
}

TEST_CASE("capture bundle alignment includes every camera and extent field", "[render][data-capture]") {
	ResourceImage first;
	first.SnapshotId = "snapshot";
	first.CaptureFrame = 4;
	first.CameraWorldFromCamera[0] = 1;
	first.CameraProjectionAvailable = true;
	first.CameraProjection[0] = 2;
	first.CameraFieldOfViewRadians = 1;
	first.CameraNearPlane = .1f;
	first.CameraFarPlane = 100;
	first.CameraCropLeft = .1f;
	first.CameraCropTop = .2f;
	first.CameraCropWidth = .7f;
	first.CameraCropHeight = .6f;
	first.CaptureWidth = 64;
	first.CaptureHeight = 48;
	CHECK(capture_record_validation::SameCaptureBundle(first, first));
	const auto rejects = [&](auto change) {
		ResourceImage candidate = first;
		change(candidate);
		CHECK_FALSE(capture_record_validation::SameCaptureBundle(first, candidate));
	};
	rejects([](ResourceImage &image) { image.SnapshotId = "other"; });
	rejects([](ResourceImage &image) { ++image.CaptureFrame; });
	rejects([](ResourceImage &image) { image.CameraWorldFromCamera[0] = 3; });
	rejects([](ResourceImage &image) { image.CameraProjectionAvailable = false; });
	rejects([](ResourceImage &image) { image.CameraProjection[0] = 3; });
	rejects([](ResourceImage &image) { image.CameraFieldOfViewRadians = 2; });
	rejects([](ResourceImage &image) { image.CameraNearPlane = .2f; });
	rejects([](ResourceImage &image) { image.CameraFarPlane = 200; });
	rejects([](ResourceImage &image) { image.CameraCropLeft = .2f; });
	rejects([](ResourceImage &image) { image.CameraCropTop = .3f; });
	rejects([](ResourceImage &image) { image.CameraCropWidth = .6f; });
	rejects([](ResourceImage &image) { image.CameraCropHeight = .5f; });
	rejects([](ResourceImage &image) { ++image.CaptureWidth; });
	rejects([](ResourceImage &image) { ++image.CaptureHeight; });
}

TEST_CASE("data capture refuses a non-rendering history policy before queueing", "[render][data-capture]") {
	Renderer renderer;
	DataCaptureRequest request{
		.SnapshotId = "snapshot-1",
		.Pipeline = engine::core::Name("capture-pipeline"),
		.CaptureNode = engine::core::Name("capture"),
		.Channels = {DataCaptureChannel::RgbLinearHdr},
		.ObjectLabels = {},
		.SemanticLabels = {},
		.PartLabels = {},
		.TemporalHistory = DataCaptureTemporalHistory::Reset,
	};
	DataCaptureTicket ticket;
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	CHECK(ticket.ResourceTokens.empty());

	request.TemporalHistory = DataCaptureTemporalHistory::Preserve;
	request.SnapshotId.clear();
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	request.SnapshotId = "snapshot-1";
	request.Channels = {DataCaptureChannel::SecondSurfaceDepth};
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	CHECK(ticket.ChannelResourceIndices.empty());
}

TEST_CASE(
	"capture record validation rejects hostile planes and status mismatches", "[render][data-capture]"
) {
	DataCaptureTicket ticket;
	ticket.CaptureNode = engine::core::Name("capture");
	ticket.Channels = {DataCaptureChannel::RgbLinearHdr};
	DataCapturePlane plane;
	plane.Channel = DataCaptureChannel::RgbLinearHdr;
	plane.CaptureNode = ticket.CaptureNode;
	plane.Status = DataCaptureStatus::Ready;
	plane.Resource = engine::core::Name("lit");
	plane.Width = 1;
	plane.Height = 1;
	plane.RowStride = 0;
	plane.Scalar = DataCaptureScalar::Float16;
	plane.ColourSpace = DataCaptureColourSpace::Linear;
	capture_record_validation::State state;
	CHECK_FALSE(capture_record_validation::Plane(ticket, plane, 1, state));
	plane.Status = DataCaptureStatus::Pending;
	plane.Resource = engine::core::Name{};
	plane.Width = 0;
	plane.Height = 0;
	plane.RowStride = 0;
	plane.Scalar = DataCaptureScalar::Unknown;
	plane.ColourSpace = DataCaptureColourSpace::Unknown;
	capture_record_validation::State pending;
	CHECK_FALSE(capture_record_validation::Plane(ticket, plane, 1, pending));
	plane.Status = DataCaptureStatus::Unsupported;
	plane.Resource = engine::core::Name{};
	plane.Scalar = DataCaptureScalar::Unknown;
	plane.ColourSpace = DataCaptureColourSpace::Unknown;
	CHECK_FALSE(capture_record_validation::Plane(ticket, plane, 1, state));
	DataCapturePlane duplicate = plane;
	duplicate.Width = 0;
	duplicate.Height = 0;
	duplicate.RowStride = 0;
	capture_record_validation::State duplicates;
	CHECK(capture_record_validation::Plane(ticket, duplicate, 1, duplicates));
	CHECK_FALSE(capture_record_validation::Plane(ticket, duplicate, 1, duplicates));

	ticket.Channels = {DataCaptureChannel::AmbientOcclusion};
	DataCapturePlane ambient;
	ambient.Channel = DataCaptureChannel::AmbientOcclusion;
	ambient.CaptureNode = ticket.CaptureNode;
	ambient.Status = DataCaptureStatus::Ready;
	ambient.Resource = engine::core::Name("occlusion");
	ambient.Width = 2;
	ambient.Height = 1;
	ambient.RowStride = 2;
	ambient.Scalar = DataCaptureScalar::UNorm8;
	ambient.ColourSpace = DataCaptureColourSpace::NotApplicable;
	ambient.Bytes = {std::byte{0}, std::byte{255}};
	ambient.Hash = engine::assets::Hasher::Of(ambient.Bytes);
	ambient.AmbientOcclusion = {
		.SourceState = AmbientOcclusionSourceState::Unavailable,
		.ProducerFrame = std::nullopt,
		.Enabled = std::nullopt,
		.SampleCount = std::nullopt,
		.RadiusWorldUnits = std::nullopt,
		.Denoiser = std::nullopt,
		.TemporalHistory = std::nullopt,
		.BackgroundValue = std::nullopt,
		.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
	};
	capture_record_validation::State ambientState;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, ambientState));
	ambient.AmbientOcclusion->Enabled = true;
	capture_record_validation::State unavailableWithFacts;
	CHECK_FALSE(capture_record_validation::Plane(ticket, ambient, 1, unavailableWithFacts));
	ambient.AmbientOcclusion = {
		.SourceState = AmbientOcclusionSourceState::Estimated,
		.ProducerFrame = 7,
		.Enabled = true,
		.SampleCount = 12,
		.RadiusWorldUnits = 0.65f,
		.Denoiser = AmbientOcclusionDenoiser::None,
		.TemporalHistory = AmbientOcclusionTemporalHistory::Disabled,
		.BackgroundValue = 1.0f,
		.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
	};
	capture_record_validation::State estimated;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, estimated));
	ambient.AmbientOcclusion->SourceState = AmbientOcclusionSourceState::ClearedDisabled;
	ambient.AmbientOcclusion->Enabled = false;
	capture_record_validation::State disabled;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, disabled));
	ambient.AmbientOcclusion = {
		.SourceState = AmbientOcclusionSourceState::ClearedNoPass,
		.ProducerFrame = 8,
		.Enabled = true,
		.SampleCount = std::nullopt,
		.RadiusWorldUnits = std::nullopt,
		.Denoiser = std::nullopt,
		.TemporalHistory = std::nullopt,
		.BackgroundValue = 1.0f,
		.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
	};
	capture_record_validation::State noPass;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, noPass));
	ambient.RowStride = 1;
	capture_record_validation::State malformedAmbient;
	CHECK_FALSE(capture_record_validation::Plane(ticket, ambient, 1, malformedAmbient));
}

TEST_CASE("second surface capture records one strict aligned provenance pair", "[render][data-capture]") {
	constexpr std::string_view suffix =
		";eligibility=built_in_plain_opaque_front_facing;invalid_depth_metres=0;"
		"validity=0_or_255;identity=unavailable;amodal_ground_truth=false";
	const std::string provenance =
		"second_surface_depth_peel/v1;source_depth=d24_unorm;equality_bias=one_source_quantum" +
		std::string(suffix);
	CHECK(capture_record_validation::ValidSecondSurfaceProvenance(provenance));
	CHECK_FALSE(
		capture_record_validation::ValidSecondSurfaceProvenance(
			"second_surface_depth_peel/v1;source_depth=d24_unorm;equality_bias=next_representable_float" +
			std::string(suffix)
		)
	);
	const auto d16 = SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_D16_UNORM);
	const auto d24 = SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT);
	const auto d32 = SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
	REQUIRE(d16);
	REQUIRE(d24);
	REQUIRE(d32);
	CHECK(d16->SourceQuantum == 1.0f / 65535.0f);
	CHECK_FALSE(d16->NextRepresentable);
	CHECK(d24->SourceQuantum == 1.0f / 16777215.0f);
	CHECK_FALSE(d24->NextRepresentable);
	CHECK(d32->SourceQuantum == 0);
	CHECK(d32->NextRepresentable);
	for (const SDL_GPUTextureFormat format :
		 {SDL_GPU_TEXTUREFORMAT_D16_UNORM, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTUREFORMAT_D32_FLOAT})
		CHECK(capture_record_validation::ValidSecondSurfaceProvenance(SecondSurfaceProvenance(format)));
	CHECK_FALSE(SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_R8_UNORM));

	DataCaptureTicket ticket;
	ticket.CaptureNode = engine::core::Name("capture");
	ticket.Channels = {
		DataCaptureChannel::SecondSurfaceDepth,
		DataCaptureChannel::SecondSurfaceValidity,
	};
	DataCapturePlane depth;
	depth.Channel = DataCaptureChannel::SecondSurfaceDepth;
	depth.CaptureNode = ticket.CaptureNode;
	depth.Status = DataCaptureStatus::Ready;
	depth.Resource = engine::core::Name("second-surface-depth");
	depth.Width = 2;
	depth.Height = 1;
	depth.RowStride = 8;
	depth.Scalar = DataCaptureScalar::Float32;
	depth.ColourSpace = DataCaptureColourSpace::NotApplicable;
	depth.Provenance = provenance;
	depth.Bytes.assign(8, std::byte{0});
	depth.Hash = engine::assets::Hasher::Of(depth.Bytes);
	DataCapturePlane validity;
	validity.Channel = DataCaptureChannel::SecondSurfaceValidity;
	validity.CaptureNode = ticket.CaptureNode;
	validity.Status = DataCaptureStatus::Ready;
	validity.Resource = engine::core::Name("second-surface-validity");
	validity.Width = 2;
	validity.Height = 1;
	validity.RowStride = 2;
	validity.Scalar = DataCaptureScalar::UNorm8;
	validity.ColourSpace = DataCaptureColourSpace::NotApplicable;
	validity.Provenance = provenance;
	validity.Bytes = {std::byte{0}, std::byte{255}};
	validity.Hash = engine::assets::Hasher::Of(validity.Bytes);

	capture_record_validation::State aligned;
	CHECK(capture_record_validation::Plane(ticket, depth, 1, aligned));
	CHECK(capture_record_validation::Plane(ticket, validity, 1, aligned));
	validity.Width = 1;
	validity.RowStride = 1;
	validity.Bytes = {std::byte{255}};
	validity.Hash = engine::assets::Hasher::Of(validity.Bytes);
	capture_record_validation::State wrongExtent;
	CHECK(capture_record_validation::Plane(ticket, depth, 1, wrongExtent));
	CHECK_FALSE(capture_record_validation::Plane(ticket, validity, 1, wrongExtent));
	validity.Status = DataCaptureStatus::Unsupported;
	validity.Resource = {};
	validity.Width = validity.Height = validity.RowStride = 0;
	validity.Scalar = DataCaptureScalar::Unknown;
	validity.ColourSpace = DataCaptureColourSpace::Unknown;
	validity.Bytes.clear();
	validity.Hash = {};
	validity.Provenance.clear();
	capture_record_validation::State mismatchedTerminal;
	CHECK(capture_record_validation::Plane(ticket, depth, 1, mismatchedTerminal));
	CHECK_FALSE(capture_record_validation::Plane(ticket, validity, 1, mismatchedTerminal));
}

namespace {
	engine::script::DataCaptureBridgeRequest Request(std::string_view instanceId = "data-world") {
		return {
			.InstanceId = std::string(instanceId),
			.SnapshotId = "snapshot-1",
			.Pipeline = "pipeline",
			.CaptureNode = "capture",
			.Channels = {"rgb_linear_hdr"},
			.TemporalHistory = "preserve",
		};
	}
	engine::script::ViewCameraMutationRequest MutationRequest(
		std::string_view instanceId = "data-world",
		std::string_view pipeline = "pipeline",
		uint64_t revision = 1,
		std::string_view snapshot = "snapshot-1",
		size_t slot = 0
	) {
		engine::script::ViewCameraMutationRequest request;
		request.InstanceId = instanceId;
		request.SnapshotId = snapshot;
		request.Pipeline = pipeline;
		request.PipelineRevision = revision;
		request.ViewSlot = slot;
		request.Lens = engine::script::ViewCameraMutationRequest::Camera{
			.FieldOfViewRadians = 0.9f, .NearPlane = 0.2f, .FarPlane = 100.0f,
			.MaxImageWidth = 0, .MaxImageHeight = 0, .ImageWidth = 0, .ImageHeight = 0
		};
		return request;
	}

	std::string PauseAndSnapshot(engine::world::Universe &worlds, engine::world::DataFactorySession &session) {
		const auto world = worlds.Create({.Name = engine::core::Name("data-world")});
		session.SetPauseParticipant(
			[world](engine::world::WorldId candidate, engine::world::DataFactoryPauseScope, bool, std::string &) {
				return candidate == world;
			}
		);
		REQUIRE(
			session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
			engine::world::DataFactoryStatus::Ok
		);
		std::string snapshot;
		REQUIRE(session.Snapshot("data-world", snapshot).Status == engine::world::DataFactoryStatus::Ok);
		return snapshot;
	}

	engine::render::ViewMutationIdentity MutationIdentity(
		std::string_view snapshot, engine::core::Name pipeline, uint64_t revision, size_t slot = 0
	) {
		return {
			.WorldName = "data-world",
			.SnapshotId = std::string(snapshot),
			.Pipeline = pipeline,
			.PipelineRevision = revision,
			.ViewSlot = slot,
		};
	}

	engine::render::View MutationView(std::string_view snapshot, engine::core::Name pipeline, size_t slot = 0) {
		engine::render::View view;
		view.WorldName = engine::core::Name("data-world");
		view.SnapshotId = snapshot;
		view.Pipeline = pipeline;
		view.Slot = slot;
		return view;
	}
}

TEST_CASE("script view.camera lifecycle is owner-pumped and reclaimable", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	for (size_t index = 0; index < MAX_DATA_FACTORY_BATCHES; ++index) {
		uint64_t ticket = 0;
		REQUIRE(bridge.QueueViewCameraMutation("data-world", MutationRequest(), ticket, detail));
		CHECK(bridge.HasPending());
		bridge.CancelViewCameraMutation("data-world", ticket);
		bridge.Pump();
		engine::script::ViewCameraMutationPoll poll;
		REQUIRE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
		CHECK(poll.Terminal);
		CHECK(poll.Status == "cancelled");
		CHECK_FALSE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
	}
}

TEST_CASE("script view.camera validates a queued snapshot on its owner", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	const engine::core::Name pipeline("mutation-owner-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t ticket = 0;
	REQUIRE(bridge.QueueViewCameraMutation("missing-world", MutationRequest("missing-world", pipeline.Text(), installed->Revision), ticket, detail));
	View view;
	view.WorldName = engine::core::Name("missing-world");
	view.Pipeline = pipeline;
	bridge.PrepareView(view);
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("missing-world", ticket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "stale");
}

TEST_CASE("script view.camera rejects an armed patch after its snapshot becomes stale", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("mutation-armed-stale-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t ticket = 0;
	REQUIRE(
		bridge.QueueViewCameraMutation(
			"data-world", MutationRequest("data-world", pipeline.Text(), installed->Revision, snapshot), ticket, detail
		)
	);
	View armed = MutationView(snapshot, pipeline);
	bridge.PrepareView(armed);
	REQUIRE(session.Resume("data-world", 0).Status == engine::world::DataFactoryStatus::Ok);
	View fresh;
	fresh.WorldName = engine::core::Name("data-world");
	fresh.Pipeline = pipeline;
	bridge.PrepareView(fresh);
	CHECK(fresh.SnapshotId.empty());
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "stale");
	CHECK_FALSE(renderer.Hooks().ConsumeViewMutation(MutationIdentity(snapshot, pipeline, installed->Revision), fresh));
}

TEST_CASE("script view.camera bridge tracks apply, restore and cancellation", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("mutation-lifecycle-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t appliedTicket = 0;
	REQUIRE(
		bridge.QueueViewCameraMutation(
			"data-world", MutationRequest("data-world", pipeline.Text(), installed->Revision, snapshot), appliedTicket, detail
		)
	);
		View armedView = MutationView(snapshot, pipeline);
		SceneTarget target{16, 16};
		armedView.Target = &target;
		armedView.World = 1;
		bridge.PrepareView(armedView);

		// A host can return before Render. The armed patch must still choose its
		// admitted snapshot for the next fresh view rather than waiting forever.
		View view;
		view.WorldName = engine::core::Name("data-world");
		view.Pipeline = pipeline;
		view.Target = &target;
		view.World = 1;
		bridge.PrepareView(view);
		CHECK(view.SnapshotId == snapshot);
		const auto identity = MutationIdentity(snapshot, pipeline, installed->Revision);
	OverlayImage overlay;
	const FrameResult appliedFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(appliedFrame.Submitted);
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("data-world", appliedTicket, poll, detail));
	CHECK_FALSE(poll.Terminal);
	CHECK(poll.Status == "pending");

	// The completed patched image may need its unmodified replacement even if
	// the scene advances before the restore view is prepared.
	REQUIRE(session.Resume("data-world", 0).Status == engine::world::DataFactoryStatus::Ok);
	bridge.PrepareView(view);
	const FrameResult restoredFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(restoredFrame.Submitted);
	bridge.Pump();
	REQUIRE(bridge.PollViewCameraMutation("data-world", appliedTicket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "applied");

	REQUIRE(
		session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	std::string cancellationSnapshot;
	REQUIRE(session.Snapshot("data-world", cancellationSnapshot).Status == engine::world::DataFactoryStatus::Ok);
	view.SnapshotId = cancellationSnapshot;
	uint64_t cancelledTicket = 0;
	REQUIRE(
		bridge.QueueViewCameraMutation(
			"data-world",
			MutationRequest("data-world", pipeline.Text(), installed->Revision, cancellationSnapshot),
			cancelledTicket,
			detail
		)
	);
	bridge.PrepareView(view);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	bridge.Pump();
	bridge.CancelViewCameraMutation("data-world", cancelledTicket);
	bridge.Pump();
	REQUIRE(bridge.PollViewCameraMutation("data-world", cancelledTicket, poll, detail));
	CHECK_FALSE(poll.Terminal);
	bridge.PrepareView(view);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	bridge.Pump();
	REQUIRE(bridge.PollViewCameraMutation("data-world", cancelledTicket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "cancelled");
}

TEST_CASE("script view.camera bridge reclaims applied slots after stale pipeline and teardown", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("mutation-stale-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t staleTicket = 0;
	REQUIRE(
		bridge.QueueViewCameraMutation(
			"data-world", MutationRequest("data-world", pipeline.Text(), installed->Revision, snapshot), staleTicket, detail
		)
	);
	View staleView = MutationView(snapshot, pipeline);
	bridge.PrepareView(staleView);
	const auto staleIdentity = MutationIdentity(snapshot, pipeline, installed->Revision);
	View prepared = staleView;
	REQUIRE(renderer.Hooks().ConsumeViewMutation(staleIdentity, prepared));
	bridge.Pump();
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("data-world", staleTicket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "stale");

	const auto current = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(current);
	for (size_t slot = 0; slot < MAX_DATA_FACTORY_BATCHES; ++slot) {
		uint64_t ticket = 0;
		REQUIRE(
			bridge.QueueViewCameraMutation(
				"data-world", MutationRequest("data-world", pipeline.Text(), current->Revision, snapshot, slot), ticket, detail
			)
		);
		View view = MutationView(snapshot, pipeline, slot);
		bridge.PrepareView(view);
		View applied = view;
		REQUIRE(renderer.Hooks().ConsumeViewMutation(MutationIdentity(snapshot, pipeline, current->Revision, slot), applied));
		bridge.Pump();
	}
	REQUIRE(bridge.TeardownInstance("data-world", detail));
	for (size_t slot = 0; slot < MAX_DATA_FACTORY_BATCHES; ++slot) {
		uint64_t ticket = 0;
		REQUIRE(
			bridge.QueueViewCameraMutation(
				"data-world", MutationRequest("data-world", pipeline.Text(), current->Revision, snapshot, slot), ticket, detail
			)
		);
		View view = MutationView(snapshot, pipeline, slot);
		bridge.PrepareView(view);
		View applied = view;
		REQUIRE(renderer.Hooks().ConsumeViewMutation(MutationIdentity(snapshot, pipeline, current->Revision, slot), applied));
		bridge.Pump();
		REQUIRE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
		CHECK_FALSE(poll.Terminal);
	}
	REQUIRE(bridge.TeardownInstance("data-world", detail));
}

TEST_CASE("script capture retains terminal tickets until release", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	const auto request = Request();
	std::array<uint64_t, 6> tickets{};
	std::string detail;
	for (uint64_t &ticket : tickets)
		REQUIRE(bridge.Queue("data-world", request, ticket, detail));
	uint64_t refused = 0;
	CHECK_FALSE(bridge.Queue("data-world", request, refused, detail));
	bridge.Cancel("data-world", tickets[2]);
	bridge.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge.Poll("data-world", tickets[2], reply, detail));
	CHECK(reply.Status == "cancelled");
	std::vector<std::byte> bytes;
	CHECK_FALSE(bridge.ReadPlane("data-world", tickets[2], "capture/3/rgb_linear_hdr", 0, 8, bytes, detail));
	REQUIRE(bridge.Release("data-world", tickets[2], detail));
	uint64_t reused = 0;
	CHECK(bridge.Queue("data-world", request, reused, detail));
}

TEST_CASE("script capture advertises the SSAO estimator channel", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	const auto capabilities = bridge.Capabilities();
	CHECK(
		std::find(capabilities.Channels.begin(), capabilities.Channels.end(), "ambient_occlusion") !=
		capabilities.Channels.end()
	);
	const size_t observationHooks = static_cast<size_t>(std::count_if(
		capabilities.HookRecords.begin(),
		capabilities.HookRecords.end(),
		[](const auto &hook) { return hook.Access == "observation"; }
	));
	REQUIRE(observationHooks == 12);
	CHECK(capabilities.HookRecords.size() == observationHooks + 1);
	for (const auto &hook : capabilities.HookRecords) {
		if (hook.Access != "observation") continue;
		CHECK(hook.Name.starts_with("data_capture."));
		CHECK(hook.SchemaVersion == 1);
		CHECK(hook.NodeKind == "capture");
		CHECK(hook.Required);
		REQUIRE(hook.Channels.size() == 1);
		CHECK(
			std::find(capabilities.Channels.begin(), capabilities.Channels.end(), hook.Channels.front()) !=
			capabilities.Channels.end()
		);
	}
	const auto mutation = std::find_if(
		capabilities.HookRecords.begin(),
		capabilities.HookRecords.end(),
		[](const auto &hook) { return hook.Name == "view.camera"; }
	);
	REQUIRE(mutation != capabilities.HookRecords.end());
	CHECK(mutation->Access == "synchronous_mutation");
	CHECK(mutation->MutatedFields.size() == 3);
	CHECK(capabilities.MaximumHooks == MAX_DATA_FACTORY_HOOKS);
	CHECK(capabilities.MaximumConnections == MAX_DATA_FACTORY_CONNECTIONS);
	CHECK(capabilities.MaximumBatches == MAX_DATA_FACTORY_BATCHES);
	CHECK(capabilities.MaximumReadbackNodes == 12);
}

TEST_CASE("script capture validates requests and isolates ticket owners", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge first(session, renderer);
	ScriptDataCaptureBridge second(session, renderer);
	std::string detail;
	uint64_t ticket = 0;

	auto malformed = Request();
	malformed.Channels = {"rgb_linear_hdr", "rgb_linear_hdr"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.CaptureNode = std::string("capture\0node", 12);
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.Channels = {"semantic_mask"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.Channels = {"second_surface_depth"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	CHECK_FALSE(first.Queue("another-world", Request(), ticket, detail));
	const auto broad = engine::script::DataCaptureBridgeRequest{
		.InstanceId = "data-world",
		.SnapshotId = "snapshot-1",
		.Pipeline = "pipeline",
		.CaptureNode = "capture",
		.Channels =
			{"rgb_linear_hdr",
			 "linear_depth",
			 "shading_normal",
			 "pbr_albedo",
			 "pbr_material",
			 "pbr_emissive",
			 "ambient_occlusion",
			 "object_ids",
			 "semantic_ids",
			 "part_ids",
			 "second_surface_depth",
			 "second_surface_validity"},
		.TemporalHistory = "preserve",
	};
	uint64_t broadTicket = 0;
	REQUIRE(first.Queue("data-world", broad, broadTicket, detail));

	uint64_t firstTicket = 0;
	uint64_t secondTicket = 0;
	REQUIRE(first.Queue("data-world", Request(), firstTicket, detail));
	REQUIRE(second.Queue("data-world", Request(), secondTicket, detail));
	first.Cancel("another-world", firstTicket);
	first.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(first.Poll("data-world", firstTicket, reply, detail));
	CHECK(reply.Status == "pending");
	CHECK_FALSE(first.Poll("another-world", firstTicket, reply, detail));
	second.Cancel("data-world", secondTicket);
	second.Pump();
	REQUIRE(second.Poll("data-world", secondTicket, reply, detail));
	CHECK(reply.Status == "cancelled");
	REQUIRE(second.Release("data-world", secondTicket, detail));
	CHECK_FALSE(second.Poll("data-world", firstTicket, reply, detail));
}

TEST_CASE("script capture synchronizes cancellation with an owner pump", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t ticket = 0;
	REQUIRE(bridge.Queue("data-world", Request(), ticket, detail));
	std::atomic<bool> started = false;
	std::thread owner([&] {
		started = true;
		for (size_t index = 0; index < 16; ++index)
			bridge.Pump();
	});
	while (!started.load()) {}
	bridge.Cancel("data-world", ticket);
	owner.join();
	bridge.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge.Poll("data-world", ticket, reply, detail));
	CHECK(reply.Status == "cancelled");
	REQUIRE(bridge.Release("data-world", ticket, detail));
}

TEST_CASE("script capture reuses capacity after sequential terminal releases", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;

	for (size_t index = 0; index < 8; ++index) {
		uint64_t ticket = 0;
		REQUIRE(bridge.Queue("data-world", Request(), ticket, detail));
		bridge.Cancel("data-world", ticket);
		bridge.Pump();
		engine::script::DataCaptureBridgePoll reply;
		REQUIRE(bridge.Poll("data-world", ticket, reply, detail));
		CHECK(reply.Status == "cancelled");
		std::vector<std::byte> bytes;
		CHECK_FALSE(bridge.ReadPlane("data-world", ticket, "capture/invalid", 0, 16, bytes, detail));
		REQUIRE(bridge.Release("data-world", ticket, detail));
	}
}

TEST_CASE("script capture tears down one instance without touching another", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t retiring = 0;
	uint64_t retained = 0;
	REQUIRE(bridge.Queue("retiring-world", Request("retiring-world"), retiring, detail));
	REQUIRE(bridge.Queue("retained-world", Request("retained-world"), retained, detail));
	REQUIRE(bridge.TeardownInstance("retiring-world", detail));
	engine::script::DataCaptureBridgePoll reply;
	CHECK_FALSE(bridge.Poll("retiring-world", retiring, reply, detail));
	CHECK(bridge.Poll("retained-world", retained, reply, detail));
	uint64_t replacement = 0;
	CHECK(bridge.Queue("retiring-world", Request("retiring-world"), replacement, detail));
	CHECK_FALSE(bridge.TeardownInstance("", detail));
}
