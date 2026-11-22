// Second surface eligibility and script capture byte retention.
#include "ResourceImageFixture.hpp"

TEST_SUITE_ID("engine.render.resourceimagescriptcapture")

TEST_CASE("second surface eligibility rejects non-built-in fragments", "[render][gpu][data-capture][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), pipeline, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("second-surface-eligibility");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}},
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	assets::MeshData backface = plane;
	backface.Indices = {2, 1, 0, 3, 2, 0};
	REQUIRE(renderer.AddMesh(core::Name("second-surface-plane"), plane));
	REQUIRE(renderer.AddMesh(core::Name("second-surface-backface"), backface));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("second-surface-white"), white));
	assets::TextureData middleHeight = white;
	middleHeight.Pixels = {std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
	REQUIRE(renderer.AddTexture(core::Name("second-surface-middle-height"), middleHeight));

	scene::DrawInstance front;
	front.Source = 1;
	front.Frame.Position = {0, 0, -3};
	front.HalfExtent = {1, 1, .01f};
	front.Mesh = core::Name("second-surface-plane");
	front.Texture = core::Name("second-surface-white");
	front.CastShadow = false;
	const auto validPixels = [&](scene::DrawInstance candidate, std::string snapshot) {
		const std::array rows{front, candidate};
		render::SceneTarget target{32, 24};
		render::View view;
		view.Pipeline = pipelineName;
		view.Target = &target;
		view.SnapshotId = std::move(snapshot);
		view.Instances = rows;
		render::DataCaptureRequest request{
			.SnapshotId = view.SnapshotId,
			.Pipeline = pipelineName,
			.CaptureNode = core::Name("data-capture"),
			.Channels =
				{render::DataCaptureChannel::SecondSurfaceDepth,
				 render::DataCaptureChannel::SecondSurfaceValidity},
			.ObjectLabels = {},
			.SemanticLabels = {},
			.PartLabels = {},
			.LocalLightIds = {},
		};
		render::DataCaptureTicket ticket;
		REQUIRE(renderer.QueueDataCapture(request, ticket));
		REQUIRE(ticket.ResourceTokens.size() == 1);
		CHECK(ticket.ChannelResourceIndices == std::vector<uint8_t>{0, 0});
		render::OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false)
					.Ran(core::Name("data-capture-second-surface")));
		render::DataCapturePoll captured;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do {
			captured = renderer.PollDataCapture(ticket);
			if (captured.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
		} while (captured.Status == render::DataCaptureStatus::Pending &&
				 std::chrono::steady_clock::now() < deadline);
		REQUIRE(captured.Status == render::DataCaptureStatus::Ready);
		REQUIRE(captured.Planes.size() == 2);
		size_t count = 0;
		for (const std::byte value : captured.Planes[1].Bytes)
			if (std::to_integer<uint8_t>(value) == 255) ++count;
		return count;
	};

	scene::DrawInstance rear = front;
	rear.Source = 2;
	rear.Frame.Position.Z = -4;
	CHECK(validPixels(rear, "eligible") > 0);
	scene::DrawInstance equal = rear;
	equal.Source = 3;
	equal.Frame.Position.Z = -3;
	CHECK(validPixels(equal, "equal") == 0);
	scene::DrawInstance custom = rear;
	custom.Source = 4;
	custom.Shader = core::Name("authored-shader");
	CHECK(validPixels(custom, "custom") == 0);
	scene::DrawInstance reversed = rear;
	reversed.Source = 5;
	reversed.Mesh = core::Name("second-surface-backface");
	CHECK(validPixels(reversed, "backface") == 0);
	scene::DrawInstance clipped = rear;
	clipped.Source = 6;
	clipped.SeamNormal = {0, 0, 1};
	clipped.SeamOffset = 0;
	CHECK(validPixels(clipped, "seam") == 0);
	scene::DrawInstance displaced = rear;
	displaced.Source = 7;
	displaced.HeightMap = core::Name("second-surface-middle-height");
	displaced.RenderFeatures.Enable |= scene::FeatureBit(scene::RenderFeature::Displacement);
	CHECK(validPixels(displaced, "displaced") > 0);
}

TEST_CASE("script capture retains copied bytes until explicit release", "[render][gpu][data-capture][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;

	world::Universe worlds;
	world::WorldSettings settings;
	settings.Name = core::Name("script-capture-world");
	const world::WorldId world = worlds.Create(settings);
	REQUIRE(world.IsValid());
	const std::string stablePipeline = "image-export-pipeline";
	const std::string runtimePipeline = stablePipeline + "#" + std::to_string(world.Index);
	InstallImageCapture(renderer, "display", true, false, false, runtimePipeline);
	world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[world](world::WorldId paused, world::DataFactoryPauseScope scope, bool, std::string &) {
			return paused == world && scope == world::DataFactoryPauseScope::AllSystems;
		}
	);
	REQUIRE(
		session.Pause("script-capture-world", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("script-capture-world", snapshot).Status == world::DataFactoryStatus::Ok);

	render::ScriptDataCaptureBridge bridge(session, renderer);
	const auto hookCapabilities = renderer.Hooks().DescribeHooks();
	const size_t observationHookCount = static_cast<size_t>(
		std::count_if(hookCapabilities.begin(), hookCapabilities.end(), [](const auto &hook) {
			return hook.Kind == render::RenderHookKind::DataCapture;
		})
	);
	REQUIRE(observationHookCount == 22);
	REQUIRE(observationHookCount + 1 == render::MAX_DATA_FACTORY_HOOKS);
	script::DataCaptureBridgeRequest request{
		.InstanceId = "script-capture-world",
		.SnapshotId = snapshot,
		.Pipeline = stablePipeline,
		.CaptureNode = "image-export",
		.Channels = {"object_ids"},
		.LocalLightIds = {},
		.TemporalHistory = "preserve",
		.IncludeSceneData = true,
		.PackedPlanes = {},
	};
	uint64_t ticket = 0;
	std::string detail;
	REQUIRE(bridge.Queue("script-capture-world", request, ticket, detail));

	render::SceneTarget target{32, 24};
	render::View view;
	view.WorldName = core::Name("script-capture-world");
	view.World = world.Index;
	view.Pipeline = core::Name(runtimePipeline);
	view.Target = &target;
	const std::array objectLabels{
		render::DataCaptureObjectLabel{1, "script/alpha"},
		render::DataCaptureObjectLabel{2, "script/packed"},
	};
	view.ObjectLabels = objectLabels;
	bridge.PrepareView(view);
	bridge.PrepareView(view);
	REQUIRE(view.SnapshotId == snapshot);
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));

	script::DataCaptureBridgePoll poll;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		bridge.Pump();
		REQUIRE(bridge.Poll("script-capture-world", ticket, poll, detail));
		if (poll.Status == "pending") SDL_Delay(1);
	} while (poll.Status == "pending" && std::chrono::steady_clock::now() < deadline);
	REQUIRE(poll.Status == "ready");
	REQUIRE(poll.Planes.size() == 1);
	REQUIRE(poll.ObjectLabels.size() == 2);
	CHECK(poll.ObjectLabels[0].Label == 1);
	CHECK(poll.ObjectLabels[0].StableId == "script/alpha");
	CHECK(poll.ObjectLabels[1].Label == 2);
	CHECK(poll.ObjectLabels[1].StableId == "script/packed");
	REQUIRE(poll.SceneSidecar);
	CHECK(poll.SceneSidecar->SnapshotId == snapshot);
	CHECK(poll.SceneSidecar->Tick == 0);
	CHECK(poll.SceneSidecar->WorldEpoch == session.Inspect("script-capture-world").WorldEpoch);
	CHECK(poll.SceneSidecar->WorldVersion == session.Inspect("script-capture-world").WorldVersion);
	CHECK(poll.SceneSidecar->StorageProfile == "lossless");
	CHECK(poll.SceneSidecar->Scene.Tag == script::ValueTag::Map);
	CHECK(poll.Planes.front().Resource != poll.Planes.front().SourceResource);
	std::vector<std::byte> bytes;
	const size_t byteCount = static_cast<size_t>(poll.Planes.front().RowStride) * poll.Planes.front().Height;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world", ticket, poll.Planes.front().Resource, 0, byteCount, bytes, detail
	));
	CHECK(bytes.size() == byteCount);
	CHECK(assets::Hasher::Of(bytes).ToHex() == poll.Planes.front().Hash);
	script::DataCaptureBridgePoll repeated;
	REQUIRE(bridge.Poll("script-capture-world", ticket, repeated, detail));
	REQUIRE(repeated.Planes.size() == 1);
	REQUIRE(repeated.SceneSidecar);
	CHECK(repeated.SceneSidecar->SnapshotId == snapshot);
	CHECK(repeated.Planes.front().Resource == poll.Planes.front().Resource);
	CHECK(repeated.Planes.front().Hash == poll.Planes.front().Hash);
	std::vector<std::byte> repeatedBytes;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world", ticket, repeated.Planes.front().Resource, 0, byteCount, repeatedBytes, detail
	));
	CHECK(repeatedBytes == bytes);
	REQUIRE(bridge.Release("script-capture-world", ticket, detail));
	CHECK_FALSE(bridge.Poll("script-capture-world", ticket, poll, detail));

	// Training compact is a retained-byte transform, never a renderer precision
	// change. The lossless and compact captures therefore share exact ID bytes.
	auto complete = [&](uint64_t captureTicket, script::DataCaptureBridgePoll &result) {
		view.SnapshotId.clear();
		bridge.PrepareView(view);
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		do {
			bridge.Pump();
			REQUIRE(bridge.Poll("script-capture-world", captureTicket, result, detail));
			if (result.Status == "pending") SDL_Delay(1);
		} while (result.Status == "pending" && std::chrono::steady_clock::now() < deadline);
		REQUIRE(result.Status == "ready");
	};
	auto plane = [](const script::DataCaptureBridgePoll &result,
					std::string_view channel) -> const script::DataCaptureBridgePlane & {
		const auto found =
			std::ranges::find_if(result.Planes, [&](const auto &item) { return item.Channel == channel; });
		REQUIRE(found != result.Planes.end());
		return *found;
	};
	script::DataCaptureBridgeRequest losslessDepth = request;
	losslessDepth.Channels = {"linear_depth", "object_ids"};
	uint64_t losslessDepthTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", losslessDepth, losslessDepthTicket, detail));
	script::DataCaptureBridgePoll losslessDepthPoll;
	complete(losslessDepthTicket, losslessDepthPoll);
	const auto &losslessDepthPlane = plane(losslessDepthPoll, "linear_depth");
	const auto &losslessIdsPlane = plane(losslessDepthPoll, "object_ids");
	CHECK(losslessDepthPlane.Scalar == "float32");
	CHECK(losslessDepthPlane.Encoding == "ieee754_binary32_le");
	CHECK(losslessIdsPlane.Scalar == "uint32");
	CHECK(losslessIdsPlane.Encoding == "uint32_le");
	std::vector<std::byte> losslessIds;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		losslessDepthTicket,
		losslessIdsPlane.Resource,
		0,
		losslessIdsPlane.ByteSize,
		losslessIds,
		detail
	));

	script::DataCaptureBridgeRequest compactDepth = losslessDepth;
	compactDepth.StorageProfile = "training_compact";
	uint64_t compactDepthTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", compactDepth, compactDepthTicket, detail));
	script::DataCaptureBridgePoll compactDepthPoll;
	complete(compactDepthTicket, compactDepthPoll);
	const auto &compactDepthPlane = plane(compactDepthPoll, "linear_depth");
	const auto &compactIdsPlane = plane(compactDepthPoll, "object_ids");
	CHECK(compactDepthPlane.Scalar == "float16");
	CHECK(compactDepthPlane.SourceScalar == "float32");
	CHECK(compactDepthPlane.Encoding == "ieee754_binary16_le");
	CHECK(compactDepthPlane.SourceResource == losslessDepthPlane.SourceResource);
	CHECK(compactDepthPlane.SourceHash == losslessDepthPlane.Hash);
	CHECK(compactDepthPlane.SourceWidth == losslessDepthPlane.Width);
	CHECK(compactDepthPlane.SourceHeight == losslessDepthPlane.Height);
	CHECK(compactDepthPlane.SourceRowStride == losslessDepthPlane.RowStride);
	CHECK(compactDepthPlane.SourceByteSize == losslessDepthPlane.ByteSize);
	CHECK(compactDepthPlane.SourceEncoding == "ieee754_binary32_le");
	CHECK(compactDepthPlane.SourceColourSpace == "not_applicable");
	CHECK(compactDepthPlane.SourceOrigin == "top_left");
	CHECK(compactDepthPlane.SourceProvenance.empty());
	REQUIRE(compactDepthPlane.MaximumAbsoluteError);
	CHECK(*compactDepthPlane.MaximumAbsoluteError <= .01);
	CHECK(compactDepthPlane.RowStride * 2 == losslessDepthPlane.RowStride);
	CHECK(compactDepthPlane.ByteSize * 2 == losslessDepthPlane.ByteSize);
	const uint64_t sidecarBytes =
		losslessDepthPoll.Profile.RetainedBytes - losslessDepthPoll.Profile.SourceBytes;
	CHECK(compactDepthPoll.Profile.SourceBytes == losslessDepthPoll.Profile.RetainedBytes - sidecarBytes);
	CHECK(compactDepthPoll.Profile.ReadbackBytes == compactDepthPoll.Profile.SourceBytes);
	CHECK(
		compactDepthPoll.Profile.RetainedBytes ==
		compactDepthPlane.ByteSize + compactIdsPlane.ByteSize + sidecarBytes
	);
	CHECK(compactDepthPoll.Profile.SourceBytes > compactDepthPoll.Profile.RetainedBytes - sidecarBytes);
	REQUIRE(compactDepthPoll.Profile.CpuFinalizationNanoseconds);
	CHECK(compactDepthPoll.Profile.FinalizationBytesPerSecond);
	CHECK(compactIdsPlane.Scalar == "uint32");
	CHECK(compactIdsPlane.SourceScalar == "uint32");
	CHECK(compactIdsPlane.Encoding == "uint32_le");
	std::vector<std::byte> compactIds;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		compactDepthTicket,
		compactIdsPlane.Resource,
		0,
		compactIdsPlane.ByteSize,
		compactIds,
		detail
	));
	CHECK(compactIds == losslessIds);
	std::vector<std::byte> compactDepthBytes;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		compactDepthTicket,
		compactDepthPlane.Resource,
		0,
		compactDepthPlane.ByteSize,
		compactDepthBytes,
		detail
	));
	CHECK(assets::Hasher::Of(compactDepthBytes).ToHex() == compactDepthPlane.Hash);
	REQUIRE(bridge.Poll("script-capture-world", compactDepthTicket, poll, detail));
	CHECK(poll.Profile.TransferBytes == compactIdsPlane.ByteSize + compactDepthPlane.ByteSize);
	CHECK(poll.Profile.TransferOperations == 2);
	REQUIRE(compactDepthPoll.SceneSidecar);
	CHECK(compactDepthPoll.SceneSidecar->StorageProfile == "training_compact");
	REQUIRE(bridge.Release("script-capture-world", losslessDepthTicket, detail));
	REQUIRE(bridge.Release("script-capture-world", compactDepthTicket, detail));
	CHECK_FALSE(bridge.Poll("script-capture-world", compactDepthTicket, poll, detail));

	// Noise is a retained RGB copy transform. Its descriptor keeps the native
	// source while object IDs remain byte-identical labels from the same render.
	script::DataCaptureBridgeRequest nativeRgb = request;
	nativeRgb.Channels = {"rgb_linear_hdr", "object_ids"};
	uint64_t nativeRgbTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", nativeRgb, nativeRgbTicket, detail));
	script::DataCaptureBridgePoll nativeRgbPoll;
	complete(nativeRgbTicket, nativeRgbPoll);
	REQUIRE(nativeRgbPoll.Status == "ready");
	const auto &nativeRgbPlane = plane(nativeRgbPoll, "rgb_linear_hdr");
	const auto &nativeNoiseIdsPlane = plane(nativeRgbPoll, "object_ids");
	std::vector<std::byte> nativeRgbBytes;
	std::vector<std::byte> nativeNoiseIds;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		nativeRgbTicket,
		nativeRgbPlane.Resource,
		0,
		nativeRgbPlane.ByteSize,
		nativeRgbBytes,
		detail
	));
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		nativeRgbTicket,
		nativeNoiseIdsPlane.Resource,
		0,
		nativeNoiseIdsPlane.ByteSize,
		nativeNoiseIds,
		detail
	));

	script::DataCaptureBridgeRequest noisyRgb = nativeRgb;
	noisyRgb.NoiseMode = "gaussian";
	noisyRgb.NoiseSeed = 17;
	noisyRgb.NoiseSigma = .1;
	uint64_t noisyRgbTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", noisyRgb, noisyRgbTicket, detail));
	script::DataCaptureBridgePoll noisyRgbPoll;
	complete(noisyRgbTicket, noisyRgbPoll);
	REQUIRE(noisyRgbPoll.Status == "ready");
	const auto &noisyRgbPlane = plane(noisyRgbPoll, "rgb_linear_hdr");
	const auto &noisyIdsPlane = plane(noisyRgbPoll, "object_ids");
	REQUIRE(noisyRgbPlane.Noise);
	CHECK(noisyRgbPlane.Noise->Algorithm == "xorshift64star_clt12_q17/v2");
	CHECK(noisyRgbPlane.Noise->Sigma == .1);
	CHECK(noisyRgbPlane.Noise->EffectiveSigmaQ24 == 1677722);
	CHECK(noisyRgbPlane.Noise->EffectiveSigma == 1677722.0 / 16777216.0);
	CHECK(noisyRgbPlane.Noise->Order == "after_storage_profile/v1");
	CHECK(noisyRgbPlane.Noise->AlphaPolicy == "preserve_exact_binary16");
	REQUIRE(noisyRgbPlane.Noise->MaximumAbsoluteError);
	CHECK(*noisyRgbPlane.Noise->MaximumAbsoluteError >= 0.0);
	CHECK(noisyRgbPlane.SourceHash == nativeRgbPlane.Hash);
	CHECK(noisyRgbPlane.SourceScalar == nativeRgbPlane.Scalar);
	CHECK(noisyRgbPlane.SourceEncoding == nativeRgbPlane.Encoding);
	CHECK(noisyRgbPlane.SourceWidth == nativeRgbPlane.Width);
	CHECK(noisyRgbPlane.SourceHeight == nativeRgbPlane.Height);
	CHECK(noisyRgbPlane.SourceRowStride == nativeRgbPlane.RowStride);
	CHECK(noisyRgbPlane.SourceByteSize == nativeRgbPlane.ByteSize);
	CHECK(noisyRgbPlane.Hash != nativeRgbPlane.Hash);
	std::vector<std::byte> noisyRgbBytes;
	std::vector<std::byte> noisyIds;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		noisyRgbTicket,
		noisyRgbPlane.Resource,
		0,
		noisyRgbPlane.ByteSize,
		noisyRgbBytes,
		detail
	));
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		noisyRgbTicket,
		noisyIdsPlane.Resource,
		0,
		noisyIdsPlane.ByteSize,
		noisyIds,
		detail
	));
	CHECK(assets::Hasher::Of(noisyRgbBytes).ToHex() == noisyRgbPlane.Hash);
	CHECK(noisyRgbBytes != nativeRgbBytes);
	CHECK(noisyIds == nativeNoiseIds);
	script::DataCaptureBridgePoll noisyRetry;
	REQUIRE(bridge.Poll("script-capture-world", noisyRgbTicket, noisyRetry, detail));
	CHECK(noisyRetry.Status == "ready");
	CHECK(plane(noisyRetry, "rgb_linear_hdr").Hash == noisyRgbPlane.Hash);
	CHECK(plane(noisyRetry, "object_ids").Hash == noisyIdsPlane.Hash);
	REQUIRE(bridge.Release("script-capture-world", nativeRgbTicket, detail));
	REQUIRE(bridge.Release("script-capture-world", noisyRgbTicket, detail));

	// Signed zero is normalized to exact zero after validation, retaining the
	// native RGB bytes while still reporting a reproducible seed policy.
	script::DataCaptureBridgeRequest signedZeroNoise = nativeRgb;
	signedZeroNoise.NoiseMode = "gaussian";
	signedZeroNoise.NoiseSeed = 0;
	signedZeroNoise.NoiseSigma = -0.0;
	uint64_t signedZeroTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", signedZeroNoise, signedZeroTicket, detail));
	script::DataCaptureBridgePoll signedZeroPoll;
	complete(signedZeroTicket, signedZeroPoll);
	REQUIRE(signedZeroPoll.Status == "ready");
	const auto &signedZeroPlane = plane(signedZeroPoll, "rgb_linear_hdr");
	REQUIRE(signedZeroPlane.Noise);
	CHECK(signedZeroPlane.Noise->Sigma == 0.0);
	CHECK(signedZeroPlane.Noise->EffectiveSigmaQ24 == 0);
	CHECK(signedZeroPlane.Noise->EffectiveSigma == 0.0);
	CHECK(signedZeroPlane.Noise->SeedStatePolicy == "zero_maps_to_0x9e3779b97f4a7c15_else_direct/v1");
	std::vector<std::byte> signedZeroBytes;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world",
		signedZeroTicket,
		signedZeroPlane.Resource,
		0,
		signedZeroPlane.ByteSize,
		signedZeroBytes,
		detail
	));
	CHECK(signedZeroBytes == nativeRgbBytes);
	REQUIRE(bridge.Release("script-capture-world", signedZeroTicket, detail));

	// Requested storage is observable before submission and after an early cancellation.
	script::DataCaptureBridgeRequest pendingCompact = request;
	pendingCompact.StorageProfile = "training_compact";
	uint64_t pendingCompactTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", pendingCompact, pendingCompactTicket, detail));
	REQUIRE(bridge.Poll("script-capture-world", pendingCompactTicket, poll, detail));
	CHECK(poll.Status == "pending");
	CHECK(poll.StorageProfile == "training_compact");
	bridge.Cancel("script-capture-world", pendingCompactTicket);
	bridge.Pump();
	REQUIRE(bridge.Poll("script-capture-world", pendingCompactTicket, poll, detail));
	CHECK(poll.Status == "cancelled");
	CHECK(poll.StorageProfile == "training_compact");
	CHECK(poll.Profile.RetainedBytes == 0);
	CHECK(poll.Profile.RetainedOperations == 0);
	REQUIRE(bridge.Release("script-capture-world", pendingCompactTicket, detail));

	// A canceled sidecar has already been captured and reserved by PrepareView.
	// Releasing it must return that reservation before a later capture arrives.
	uint64_t cancelledSidecarTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", request, cancelledSidecarTicket, detail));
	view.SnapshotId.clear();
	bridge.PrepareView(view);
	REQUIRE(view.SnapshotId == snapshot);
	bridge.Cancel("script-capture-world", cancelledSidecarTicket);
	bridge.Pump();
	REQUIRE(bridge.Poll("script-capture-world", cancelledSidecarTicket, poll, detail));
	CHECK(poll.Status == "cancelled");
	CHECK_FALSE(poll.SceneSidecar);
	REQUIRE(bridge.Release("script-capture-world", cancelledSidecarTicket, detail));

	uint64_t recoveredSidecarTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", request, recoveredSidecarTicket, detail));
	view.SnapshotId.clear();
	bridge.PrepareView(view);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	do {
		bridge.Pump();
		REQUIRE(bridge.Poll("script-capture-world", recoveredSidecarTicket, poll, detail));
		if (poll.Status == "pending") SDL_Delay(1);
	} while (poll.Status == "pending" && std::chrono::steady_clock::now() < deadline);
	REQUIRE(poll.Status == "ready");
	REQUIRE(poll.SceneSidecar);
	REQUIRE(bridge.Release("script-capture-world", recoveredSidecarTicket, detail));

	// The process-local view key stays supported for older internal callers.
	request.Pipeline = runtimePipeline;
	request.Channels = {"rgb_linear_hdr"};
	uint64_t rgbTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", request, rgbTicket, detail));
	bridge.PrepareView(view);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	do {
		bridge.Pump();
		REQUIRE(bridge.Poll("script-capture-world", rgbTicket, poll, detail));
		if (poll.Status == "pending") SDL_Delay(1);
	} while (poll.Status == "pending" && std::chrono::steady_clock::now() < deadline);
	REQUIRE(poll.Status == "ready");
	CHECK(poll.ObjectLabels.empty());
	REQUIRE(bridge.Release("script-capture-world", rgbTicket, detail));

	// A runtime key from another world is neither the stable profile nor this view's key.
	request.Pipeline = stablePipeline + "#" + std::to_string(world.Index + 1);
	uint64_t rejectedTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", request, rejectedTicket, detail));
	view.SnapshotId.clear();
	bridge.PrepareView(view);
	CHECK(view.SnapshotId.empty());
	REQUIRE(bridge.Poll("script-capture-world", rejectedTicket, poll, detail));
	CHECK(poll.Status == "pending");
	bridge.Cancel("script-capture-world", rejectedTicket);
	bridge.Pump();
	REQUIRE(bridge.Poll("script-capture-world", rejectedTicket, poll, detail));
	CHECK(poll.Status == "cancelled");
	CHECK_FALSE(poll.SceneSidecar);
	REQUIRE(bridge.Release("script-capture-world", rejectedTicket, detail));
	CHECK_FALSE(bridge.Poll("script-capture-world", rejectedTicket, poll, detail));

	// Shutdown after admission must publish the binder's terminal bundle to the
	// bridge, so a script ticket cannot remain pending after device teardown.
	request.Pipeline = stablePipeline;
	request.Channels = {"object_ids"};
	uint64_t shutdownTicket = 0;
	REQUIRE(bridge.Queue("script-capture-world", request, shutdownTicket, detail));
	view.SnapshotId.clear();
	bridge.PrepareView(view);
	REQUIRE(view.SnapshotId == snapshot);
	renderer.Shutdown();
	bridge.Pump();
	REQUIRE(bridge.Poll("script-capture-world", shutdownTicket, poll, detail));
	CHECK(poll.Status == "cancelled");
	REQUIRE(bridge.Release("script-capture-world", shutdownTicket, detail));
}
