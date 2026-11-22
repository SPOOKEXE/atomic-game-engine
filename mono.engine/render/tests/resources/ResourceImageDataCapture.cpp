// Data capture: noise, R8 provenance, source depth and normal planes,
// snapshot binding and mesh UV interpolation.
#include "ResourceImageFixture.hpp"

TEST_SUITE_ID("engine.render.resourceimagedatacapture")

TEST_CASE("captured RGB Gaussian noise is deterministic and preserves alpha", "[render][data-capture]") {
	using namespace engine::render::data_capture_compact;
	auto half = [](float value) {
		uint16_t bits = 0;
		REQUIRE(Float32ToFloat16(value, bits));
		return bits;
	};
	auto append = [](std::vector<std::byte> &bytes, uint16_t value) {
		bytes.push_back(static_cast<std::byte>(value & 0xff));
		bytes.push_back(static_cast<std::byte>(value >> 8));
	};
	std::vector<std::byte> finite;
	append(finite, half(1.0f));
	append(finite, half(-.5f));
	append(finite, half(.25f));
	append(finite, half(.75f));
	std::vector<std::byte> first = finite;
	std::vector<std::byte> second = finite;
	RgbNoiseResult firstResult;
	RgbNoiseResult secondResult;
	REQUIRE(ApplyGaussianRgbFloat16(first, 1, 1, 8, 17, .25, firstResult));
	REQUIRE(ApplyGaussianRgbFloat16(second, 1, 1, 8, 17, .25, secondResult));
	CHECK(first == second);
	CHECK(
		first == std::vector<std::byte>{
					 std::byte{0x18},
					 std::byte{0x3b},
					 std::byte{0xc0},
					 std::byte{0xb7},
					 std::byte{0x67},
					 std::byte{0x33},
					 std::byte{0x00},
					 std::byte{0x3a}
				 }
	);
	CHECK(firstResult.EffectiveSigmaQ24 == 4194304);
	CHECK(firstResult.EffectiveSigma == .25);
	std::vector<std::byte> seedZero = finite;
	seedZero.insert(seedZero.end(), finite.begin(), finite.end());
	RgbNoiseResult seedZeroResult;
	REQUIRE(ApplyGaussianRgbFloat16(seedZero, 2, 1, 16, 0, .25, seedZeroResult));
	CHECK(
		seedZero == std::vector<std::byte>{
						std::byte{0x0b},
						std::byte{0x3c},
						std::byte{0x0a},
						std::byte{0xba},
						std::byte{0x94},
						std::byte{0x32},
						std::byte{0x00},
						std::byte{0x3a},
						std::byte{0x42},
						std::byte{0x3b},
						std::byte{0x85},
						std::byte{0xb6},
						std::byte{0x62},
						std::byte{0x30},
						std::byte{0x00},
						std::byte{0x3a}
					}
	);
	CHECK(
		std::vector<std::byte>(seedZero.begin(), seedZero.begin() + 6) !=
		std::vector<std::byte>(seedZero.begin() + 8, seedZero.begin() + 14)
	);
	REQUIRE(firstResult.MaximumAbsoluteError);
	CHECK(*firstResult.MaximumAbsoluteError == .11328125);

	const int savedRounding = std::fegetround();
	if (savedRounding != -1 && std::fesetround(FE_UPWARD) == 0) {
		std::vector<std::byte> upward = finite;
		RgbNoiseResult upwardResult;
		REQUIRE(ApplyGaussianRgbFloat16(upward, 1, 1, 8, 17, .25, upwardResult));
		CHECK(upward == first);
		CHECK(upwardResult.MaximumAbsoluteError == firstResult.MaximumAbsoluteError);
		REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
		std::vector<std::byte> downward = finite;
		RgbNoiseResult downwardResult;
		REQUIRE(ApplyGaussianRgbFloat16(downward, 1, 1, 8, 17, .25, downwardResult));
		CHECK(downward == first);
		CHECK(downwardResult.MaximumAbsoluteError == firstResult.MaximumAbsoluteError);
		REQUIRE(std::fesetround(savedRounding) == 0);
	}

	std::vector<std::byte> exceptional;
	append(exceptional, half(1.0f));
	append(exceptional, 0x7e00); // NaN is copied, never perturbed.
	append(exceptional, 0x7c00); // Infinity is copied, never perturbed.
	append(exceptional, half(.75f));
	const std::vector<std::byte> alpha{exceptional.begin() + 6, exceptional.begin() + 8};
	RgbNoiseResult exceptionalResult;
	REQUIRE(ApplyGaussianRgbFloat16(exceptional, 1, 1, 8, 17, .25, exceptionalResult));
	CHECK(
		std::vector<std::byte>(exceptional.begin() + 2, exceptional.begin() + 6) ==
		std::vector<std::byte>{std::byte{0x00}, std::byte{0x7e}, std::byte{0x00}, std::byte{0x7c}}
	);
	CHECK(std::vector<std::byte>(exceptional.begin() + 6, exceptional.begin() + 8) == alpha);
	CHECK(exceptionalResult.ValueClassification == "contains_infinity_and_nan");

	std::vector<std::byte> padded(16, std::byte{0xa5});
	std::copy(finite.begin(), finite.end(), padded.begin());
	padded[0] = std::byte{0xff};
	padded[1] = std::byte{0x7b};
	RgbNoiseResult paddedResult;
	REQUIRE(ApplyGaussianRgbFloat16(padded, 1, 1, 16, 1, 64.0, paddedResult));
	CHECK(std::all_of(padded.begin() + 8, padded.end(), [](std::byte value) {
		return value == std::byte{0xa5};
	}));
	CHECK(
		(uint16_t(std::to_integer<uint8_t>(padded[0])) | uint16_t(std::to_integer<uint8_t>(padded[1]))
															 << 8) == 0x7bff
	); // Clamp at binary16 maximum.

	std::vector<std::byte> exhaustive;
	exhaustive.reserve(65536 * 8);
	for (uint32_t bits = 0; bits <= 0xffff; ++bits) {
		for (size_t component = 0; component < 4; ++component)
			append(exhaustive, static_cast<uint16_t>(bits));
	}
	const std::vector<std::byte> zeroSource = exhaustive;
	RgbNoiseResult zeroResult;
	REQUIRE(ApplyGaussianRgbFloat16(exhaustive, 65536, 1, 65536 * 8, 17, 0.0, zeroResult));
	CHECK(exhaustive == zeroSource);
	REQUIRE(zeroResult.MaximumAbsoluteError);
	CHECK(*zeroResult.MaximumAbsoluteError == 0.0);
	std::vector<std::byte> negativeZero = zeroSource;
	RgbNoiseResult negativeZeroResult;
	REQUIRE(ApplyGaussianRgbFloat16(negativeZero, 65536, 1, 65536 * 8, 0, -0.0, negativeZeroResult));
	CHECK(negativeZero == zeroSource);
	CHECK(negativeZeroResult.EffectiveSigmaQ24 == 0);
	CHECK(negativeZeroResult.EffectiveSigma == 0.0);

	RgbNoiseResult malformedResult;
	CHECK_FALSE(ApplyGaussianRgbFloat16(finite, UINT32_MAX, 2, UINT32_MAX, 17, .25, malformedResult));
	CHECK_FALSE(ApplyGaussianRgbFloat16(finite, 1, 1, 7, 17, .25, malformedResult));
}

TEST_CASE("custom R8 capture does not inherit SSAO facts", "[render][resourceimage]") {
	const engine::render::AmbientOcclusionProvenance builtIn{
		.SourceState = engine::render::AmbientOcclusionSourceState::Estimated,
		.ProducerFrame = 7,
		.Enabled = true,
		.SampleCount = 12,
		.RadiusWorldUnits = 0.65f,
		.Denoiser = engine::render::AmbientOcclusionDenoiser::None,
		.TemporalHistory = engine::render::AmbientOcclusionTemporalHistory::Disabled,
		.BackgroundValue = 1.0f,
		.BackgroundClassification = engine::render::AmbientOcclusionBackgroundClassification::Unavailable
	};
	const auto custom = engine::render::CapturedAmbientOcclusion(
		engine::render::ResourceImageFormat::R8_UNorm, false, builtIn
	);
	REQUIRE(custom);
	CHECK(custom->SourceState == engine::render::AmbientOcclusionSourceState::Unavailable);
	CHECK_FALSE(custom->ProducerFrame);
	CHECK_FALSE(custom->Enabled);
	CHECK_FALSE(custom->SampleCount);
	CHECK_FALSE(custom->RadiusWorldUnits);
	CHECK_FALSE(custom->Denoiser);
	CHECK_FALSE(custom->TemporalHistory);
	CHECK_FALSE(custom->BackgroundValue);
	CHECK(
		custom->BackgroundClassification ==
		engine::render::AmbientOcclusionBackgroundClassification::Unavailable
	);
}

TEST_CASE("default data capture records source depth and normal planes", "[render][gpu][resourceimage][.]") {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph pipeline;
	core::Name offender;
	auto document = graph::DefaultPbrDataCaptureDocument();
	for (const char *resource : {"lighting-baseline", "directional-response"}) {
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = core::Name(std::string("background-") + resource),
			 .NodeKind = core::Name("capture"),
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads, .Target = core::Name(resource), .Key = core::Name("source")}
		);
	}
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	const auto counter = [](std::string_view name) {
		for (const auto &entry : core::Metrics::Snapshot().Counters)
			if (entry.Name == core::Name(name)) return entry.Value;
		return 0.;
	};
	const core::Name pipelineName("default-data-capture");
	const core::Name captureNode("data-capture");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	render::SceneTarget target{33, 25};
	render::View view;
	view.Pipeline = pipelineName;
	view.Target = &target;
	view.WorldName = core::Name("fixture/world-a");
	view.SnapshotId = "render-observation-snapshot";
	view.CameraFrame.Position = {3, 4, 5};
	view.OverrideLighting = true;
	view.Lighting.FogColor = {0.5f, 0.25f, 0.75f};
	const glm::mat4 expectedCamera = view.CameraFrame.ToMatrix();
	const render::ResourceImageRequest request{
		.Token = 1,
		.Pipeline = pipelineName,
		.Node = captureNode,
		.ViewSlot = 0,
		.ExpectedSnapshotId = view.SnapshotId,
	};
	REQUIRE(renderer.RequestResourceImage(request));
	render::OverlayImage overlay;
	const render::ResourceImageRequest baselineRequest{
		9, pipelineName, core::Name("background-lighting-baseline"), 0
	};
	const render::ResourceImageRequest directionalRequest{
		10, pipelineName, core::Name("background-directional-response"), 0
	};
	REQUIRE(renderer.RequestResourceImage(baselineRequest));
	REQUIRE(renderer.RequestResourceImage(directionalRequest));
	const auto depthClearsBefore = counter("render.empty_depth.clears");
	const auto lightingClearsBefore = counter("render.empty_lighting.clears");
	const auto emptyFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(emptyFrame.Ran(captureNode));
	CHECK(counter("render.empty_depth.clears") - depthClearsBefore == 1);
	CHECK(counter("render.empty_lighting.clears") - lightingClearsBefore == 1);
	const auto backgrounds = AwaitImageGroup(renderer, std::array<uint64_t, 2>{9, 10});
	for (size_t index = 0; index < backgrounds.size(); ++index) {
		const auto &background = backgrounds[index];
		REQUIRE(background.Status == render::ResourceImageStatus::Ok);
		REQUIRE(background.Format == render::ResourceImageFormat::RGBA32_Float);
		REQUIRE(background.Pixels.size() == size_t(target.Width) * target.Height * 16);
		std::array<float, 4> expected{0.f, 0.f, 0.f, 1.f};
		if (index == 0) {
			expected[0] = render::WorkingFromDisplay(0.5f);
			expected[1] = render::WorkingFromDisplay(0.25f);
			expected[2] = render::WorkingFromDisplay(0.75f);
		}
		core::ByteReader pixels(background.Pixels);
		bool constant = true;
		while (!pixels.AtEnd())
			for (const float channel : expected)
				constant &= pixels.ReadFloat() == channel;
		CHECK(constant);
	}
	// The graph has submitted. Mutating this caller-owned View must not change
	// the observation that the asynchronous readback later returns.
	view.WorldName = core::Name("fixture/world-b");
	view.SnapshotId = "later-snapshot";
	view.CameraFrame.Position = {9, 8, 7};
	const auto image = AwaitImage(renderer, request.Token);
	CHECK(image.Status == render::ResourceImageStatus::Ok);
	REQUIRE(image.Observation);
	const auto &observation = *image.Observation;
	CHECK(render::RenderObservationHookName(observation.Hook) == "data_capture");
	CHECK(observation.Pipeline == pipelineName);
	CHECK(observation.Node == captureNode);
	CHECK(observation.WorldName == core::Name("fixture/world-a"));
	CHECK(observation.ViewSlot == 0);
	CHECK(observation.SnapshotId == "render-observation-snapshot");
	CHECK(observation.Frame == image.CaptureFrame);
	CHECK(observation.Camera.Width == target.Width);
	CHECK(observation.Camera.Height == target.Height);
	for (size_t column = 0; column < 4; ++column)
		for (size_t row = 0; row < 4; ++row)
			CHECK(observation.Camera.WorldFromCamera[column * 4 + row] == expectedCamera[column][row]);
	CHECK(observation.ReadCount == 3);
	CHECK(observation.ReadCount <= render::MAX_RENDER_OBSERVATION_RESOURCES);
	CHECK(observation.ReadResources[0] == core::Name("display"));
	CHECK(observation.ReadResources[1] == core::Name("linear-depth"));
	CHECK(observation.ReadResources[2] == core::Name("normal"));
	CHECK(observation.WriteCount == 0);
	CHECK(observation.WriteCount <= render::MAX_RENDER_OBSERVATION_RESOURCES);
	CHECK(image.Width == target.Width);
	CHECK(image.Height == target.Height);
	CHECK(image.DepthResource == core::Name("linear-depth"));
	CHECK(image.NormalResource == core::Name("normal"));
	REQUIRE(image.Depth.size() == size_t(target.Width) * target.Height * 4);
	REQUIRE(image.Normal.size() == size_t(target.Width) * target.Height * 4);
	core::ByteReader backgroundDepth(image.Depth);
	bool farBackground = true;
	while (!backgroundDepth.AtEnd())
		farBackground &= backgroundDepth.ReadFloat() == view.Camera.FarPlane;
	CHECK(farBackground);
	CHECK(image.AmbientResponse.empty());
	CHECK(image.LightingBaseline.empty());

	const render::ResourceImageRequest ambient{
		2, pipelineName, core::Name("data-capture-ambient-occlusion"), 0
	};
	REQUIRE(renderer.RequestResourceImage(ambient));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(ambient.Node));
	const auto occlusion = AwaitImage(renderer, ambient.Token);
	CHECK(occlusion.Status == render::ResourceImageStatus::Ok);
	CHECK(occlusion.Resource == core::Name("occlusion"));
	CHECK(occlusion.Format == render::ResourceImageFormat::R8_UNorm);
	CHECK(occlusion.Width == std::max(1u, target.Width / 2));
	CHECK(occlusion.Height == std::max(1u, target.Height / 2));
	CHECK(occlusion.RowStride == occlusion.Width);
	CHECK(occlusion.Pixels.size() == size_t(occlusion.Width) * occlusion.Height);
	REQUIRE(occlusion.AmbientOcclusion);
	CHECK(occlusion.AmbientOcclusion->SourceState == render::AmbientOcclusionSourceState::ClearedNoPass);
	CHECK(occlusion.AmbientOcclusion->ProducerFrame == occlusion.CaptureFrame);
	CHECK(occlusion.AmbientOcclusion->Enabled == true);
	CHECK_FALSE(occlusion.AmbientOcclusion->SampleCount);
	CHECK_FALSE(occlusion.AmbientOcclusion->RadiusWorldUnits);
	CHECK_FALSE(occlusion.AmbientOcclusion->Denoiser);
	CHECK_FALSE(occlusion.AmbientOcclusion->TemporalHistory);
	CHECK(occlusion.AmbientOcclusion->BackgroundValue == 1.0f);
	CHECK(
		occlusion.AmbientOcclusion->BackgroundClassification ==
		render::AmbientOcclusionBackgroundClassification::Unavailable
	);

	CHECK(std::all_of(occlusion.Pixels.begin(), occlusion.Pixels.end(), [](std::byte pixel) {
		return pixel == std::byte{255};
	}));
	// The output capture remains live on an unchanged scene while SSAO stays in
	// the PBR slot. Its download is newer, but its producer is not.
	view.Damage.Scene = false;
	const render::ResourceImageRequest cached{
		6, pipelineName, core::Name("data-capture-ambient-occlusion"), 0
	};
	REQUIRE(renderer.RequestResourceImage(cached));
	const render::FrameResult cachedFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(cachedFrame.Ran(cached.Node));
	CHECK_FALSE(cachedFrame.Ran(core::Name("ssao")));
	const auto reused = AwaitImage(renderer, cached.Token);
	REQUIRE(reused.AmbientOcclusion);
	CHECK(reused.CaptureFrame > occlusion.CaptureFrame);
	CHECK(reused.AmbientOcclusion->ProducerFrame == occlusion.AmbientOcclusion->ProducerFrame);
	CHECK(reused.AmbientOcclusion->ProducerFrame < reused.CaptureFrame);

	view.Damage.Scene = true;
	scene::DrawInstance wall;
	wall.Source = 1;
	wall.Frame.Position = view.CameraFrame.Position + core::Vector3{0, 0, -4};
	wall.HalfExtent = {2, 2, 0.05f};
	wall.CastShadow = false;
	view.Instances = std::span(&wall, 1);
	const render::ResourceImageRequest geometryAmbient{
		8, pipelineName, core::Name("data-capture-ambient-occlusion"), 0
	};
	REQUIRE(renderer.RequestResourceImage(geometryAmbient));
	const render::ResourceImageRequest geometryDepth{11, pipelineName, captureNode, 0};
	REQUIRE(renderer.RequestResourceImage(geometryDepth));
	const auto depthClears = counter("render.empty_depth.clears");
	const auto lightingClears = counter("render.empty_lighting.clears");
	const auto geometryFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(geometryFrame.Ran(geometryAmbient.Node));
	CHECK(geometryFrame.DrawCalls >= emptyFrame.DrawCalls + 4);
	CHECK(counter("render.empty_depth.clears") == depthClears);
	CHECK(counter("render.empty_lighting.clears") == lightingClears);
	const auto visibleWall = AwaitImage(renderer, geometryDepth.Token);
	REQUIRE(visibleWall.Status == render::ResourceImageStatus::Ok);
	core::ByteReader wallDepth(visibleWall.Depth);
	float nearest = view.Camera.FarPlane;
	while (!wallDepth.AtEnd())
		nearest = std::min(nearest, wallDepth.ReadFloat());
	CHECK(nearest == Catch::Approx(3.95f).margin(0.01f));
	const auto estimated = AwaitImage(renderer, geometryAmbient.Token);
	REQUIRE(estimated.AmbientOcclusion);
	CHECK(estimated.AmbientOcclusion->SourceState == render::AmbientOcclusionSourceState::Estimated);
	CHECK(estimated.AmbientOcclusion->Enabled == true);
	CHECK(estimated.AmbientOcclusion->SampleCount == 12);
	CHECK(estimated.AmbientOcclusion->RadiusWorldUnits == 0.65f);
	view.Instances = {};

	view.Damage.Scene = true;
	view.OverrideLighting = true;
	view.Lighting.RenderFeatures.Disable |= scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	view.Camera.RenderFeatures.Disable |= scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	const render::ResourceImageRequest disabled{
		4, pipelineName, core::Name("data-capture-ambient-occlusion"), 0
	};
	REQUIRE(renderer.RequestResourceImage(disabled));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(disabled.Node));
	const auto cleared = AwaitImage(renderer, disabled.Token);
	REQUIRE(cleared.AmbientOcclusion);
	CHECK(cleared.AmbientOcclusion->SourceState == render::AmbientOcclusionSourceState::ClearedDisabled);
	CHECK(cleared.AmbientOcclusion->ProducerFrame == cleared.CaptureFrame);
	CHECK(cleared.AmbientOcclusion->Enabled == false);
	CHECK(cleared.AmbientOcclusion->SampleCount == 12);
	CHECK(cleared.AmbientOcclusion->RadiusWorldUnits == 0.65f);

	graph::PipelineDocument noPassDocument = graph::DefaultPbrDataCaptureDocument();
	noPassDocument.Record({.Kind = graph::EditKind::Enable, .Name = core::Name("ssao"), .Enabled = false});
	graph::RenderGraph noPassPipeline;
	REQUIRE(graph::Build(noPassDocument, noPassPipeline, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name noPassName("default-data-capture-no-ssao");
	REQUIRE(renderer.SetPipeline(noPassName, noPassPipeline));
	view.Pipeline = noPassName;
	view.Lighting.RenderFeatures.Disable &= ~scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	view.Camera.RenderFeatures.Disable &= ~scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	const render::ResourceImageRequest noPass{5, noPassName, core::Name("data-capture-ambient-occlusion"), 0};
	REQUIRE(renderer.RequestResourceImage(noPass));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(noPass.Node));
	const auto noPassOcclusion = AwaitImage(renderer, noPass.Token);
	REQUIRE(noPassOcclusion.AmbientOcclusion);
	CHECK(
		noPassOcclusion.AmbientOcclusion->SourceState == render::AmbientOcclusionSourceState::ClearedNoPass
	);
	CHECK(noPassOcclusion.AmbientOcclusion->ProducerFrame == noPassOcclusion.CaptureFrame);
	CHECK(noPassOcclusion.AmbientOcclusion->Enabled == true);
	CHECK_FALSE(noPassOcclusion.AmbientOcclusion->SampleCount);
	CHECK_FALSE(noPassOcclusion.AmbientOcclusion->RadiusWorldUnits);
	CHECK_FALSE(noPassOcclusion.AmbientOcclusion->Denoiser);
	CHECK_FALSE(noPassOcclusion.AmbientOcclusion->TemporalHistory);

	view.Camera.RenderFeatures.Disable |= scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	const render::ResourceImageRequest noPassDisabled{
		7, noPassName, core::Name("data-capture-ambient-occlusion"), 0
	};
	REQUIRE(renderer.RequestResourceImage(noPassDisabled));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(noPassDisabled.Node));
	const auto noPassDisabledOcclusion = AwaitImage(renderer, noPassDisabled.Token);
	REQUIRE(noPassDisabledOcclusion.AmbientOcclusion);
	CHECK(
		noPassDisabledOcclusion.AmbientOcclusion->SourceState ==
		render::AmbientOcclusionSourceState::ClearedNoPass
	);
	CHECK(noPassDisabledOcclusion.AmbientOcclusion->Enabled == false);

	// Zero exported depth uses a separate graph texture. Native lighting depth
	// retains its far background, but this recording has no proof to skip its shader.
	graph::PipelineDocument zeroDocument;
	for (const auto &edit : document.Edits()) {
		zeroDocument.Record(edit);
		if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("depth-linearise"))
			zeroDocument.Record(
				{.Kind = graph::EditKind::Set, .Key = core::Name("background"), .Value = "zero"}
			);
	}
	graph::RenderGraph zeroPipeline;
	REQUIRE(graph::Build(zeroDocument, zeroPipeline, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name zeroName("default-data-capture-zero-background");
	REQUIRE(renderer.SetPipeline(zeroName, zeroPipeline));
	view.Pipeline = zeroName;
	view.Camera.RenderFeatures.Disable &= ~scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	view.Lighting.RenderFeatures.Disable &= ~scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	const render::ResourceImageRequest zeroRequest{12, zeroName, captureNode, 0};
	const render::ResourceImageRequest zeroBaseline{
		13, zeroName, core::Name("background-lighting-baseline"), 0
	};
	REQUIRE(renderer.RequestResourceImage(zeroRequest));
	REQUIRE(renderer.RequestResourceImage(zeroBaseline));
	const auto zeroDepthBefore = counter("render.empty_depth.clears");
	const auto zeroLightingBefore = counter("render.empty_lighting.clears");
	const auto zeroFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(zeroFrame.Ran(captureNode));
	CHECK(counter("render.empty_depth.clears") - zeroDepthBefore == 1);
	CHECK(counter("render.empty_lighting.clears") == zeroLightingBefore);
	// The unproven background needs both deferred lighting and the sky draw.
	CHECK_FALSE(emptyFrame.Ran(core::Name("sky")));
	CHECK(zeroFrame.Ran(core::Name("sky")));
	CHECK(zeroFrame.DrawCalls == emptyFrame.DrawCalls + 2);
	const auto zeroImages = AwaitImageGroup(renderer, std::array<uint64_t, 2>{12, 13});
	REQUIRE(zeroImages[0].Status == render::ResourceImageStatus::Ok);
	REQUIRE(zeroImages[1].Status == render::ResourceImageStatus::Ok);
	core::ByteReader zeroDepth(zeroImages[0].Depth);
	bool zeroDepthPixels = true;
	while (!zeroDepth.AtEnd())
		zeroDepthPixels &= zeroDepth.ReadFloat() == 0.f;
	CHECK(zeroDepthPixels);
	// The zero export must not replace the native lighting background.
	CHECK(zeroImages[1].Pixels == backgrounds[0].Pixels);

	const render::ResourceImageRequest residentAmbient{
		3,
		pipelineName,
		core::Name("data-capture-ambient-occlusion"),
		0,
		render::ResourceImageDelivery::Resident
	};
	CHECK_FALSE(renderer.RequestResourceImage(residentAmbient));
	CHECK_FALSE(renderer.CanPublishResourceImage(residentAmbient.Token, occlusion.Width, occlusion.Height));

	graph::PipelineDocument malformed = graph::DefaultPbrDataCaptureDocument();
	malformed.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = core::Name("unpaired-ambient-capture"),
		 .NodeKind = core::Name("capture"),
		 .Scope = graph::NodeScope::Frame}
	);
	for (const auto &[name, port] : std::array<std::pair<const char *, const char *>, 4>{
			 {{"lit", "source"},
			  {"linear-depth", "depth"},
			  {"normal", "normal"},
			  {"albedo", "ambient-response"}}
		 }) {
		malformed.Record(
			{.Kind = graph::EditKind::Reads, .Target = core::Name(name), .Key = core::Name(port)}
		);
	}
	graph::RenderGraph malformedPipeline;
	REQUIRE(graph::Build(malformed, malformedPipeline, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name malformedName("unpaired-ambient-capture-pipeline");
	REQUIRE(renderer.SetPipeline(malformedName, malformedPipeline));
	CHECK_FALSE(renderer.RequestResourceImage({2, malformedName, core::Name("unpaired-ambient-capture"), 0}));
}

TEST_CASE(
	"default data capture binds copied planes to the rendered snapshot", "[render][gpu][data-capture][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), pipeline, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("default-data-capture-snapshot");
	const core::Name captureNode("data-capture");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	render::SceneTarget target{32, 24};
	render::View view;
	view.Pipeline = pipelineName;
	view.Target = &target;
	view.SnapshotId = "snapshot-render-1";
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}},
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("object-id-plane"), plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("object-id-white"), white));
	assets::TextureData packedPbr;
	packedPbr.Width = packedPbr.Height = 1;
	packedPbr.Format = assets::TextureFormat::RGBA8_LINEAR;
	packedPbr.Pixels = {std::byte{64}, std::byte{128}, std::byte{192}, std::byte{255}};
	REQUIRE(renderer.AddTexture(core::Name("object-id-orm"), packedPbr));
	assets::TextureData alphaSplit;
	alphaSplit.Width = 2;
	alphaSplit.Height = 1;
	alphaSplit.Format = assets::TextureFormat::RGBA8;
	alphaSplit.Pixels.assign(8, std::byte{255});
	alphaSplit.Pixels[3] = std::byte{0};
	REQUIRE(renderer.AddTexture(core::Name("second-surface-alpha-split"), alphaSplit));
	scene::DrawInstance labelled;
	labelled.Source = 1;
	labelled.Frame.Position = {-1, 0, -4};
	labelled.HalfExtent = {.5f, .5f, .01f};
	labelled.Mesh = core::Name("object-id-plane");
	labelled.Texture = core::Name("object-id-white");
	labelled.ObjectLabel = 1;
	labelled.SemanticLabel = 1;
	labelled.PartLabel = 1;
	labelled.CastShadow = false;
	scene::DrawInstance unlabelled = labelled;
	unlabelled.Source = 2;
	unlabelled.Frame.Position = {0, 0, -3.5f};
	unlabelled.ObjectLabel = 0;
	unlabelled.Texture = core::Name("second-surface-alpha-split");
	unlabelled.Alpha = scene::AlphaMode::Transparency;
	unlabelled.AlphaCutoff = .5f;
	scene::DrawInstance centreRear = labelled;
	centreRear.Source = 4;
	centreRear.Frame.Position = {0, 0, -4.5f};
	centreRear.ObjectLabel = 0;
	scene::DrawInstance transparentFront = labelled;
	transparentFront.Source = 5;
	transparentFront.Frame.Position = {-1.25f, 0, -5};
	transparentFront.Transparency = .5f;
	scene::DrawInstance nativeRear = labelled;
	nativeRear.Source = 7;
	nativeRear.Frame.Position = {-1, 0, -6};

	scene::EditableMesh editable;
	editable.Positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
	editable.Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
	editable.UVs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
	editable.Colours = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
	editable.Alphas = {1, 1, 1, 1};
	editable.Indices = {0, 1, 2, 0, 2, 3};
	editable.Packing.Attributes = static_cast<uint8_t>(scene::EditablePackingAttribute::Position) |
								  static_cast<uint8_t>(scene::EditablePackingAttribute::Normal) |
								  static_cast<uint8_t>(scene::EditablePackingAttribute::UV);
	editable.Packing.Format = scene::EditablePackingFormat::Unsigned16;
	editable.Packing.Minimum = -1;
	editable.Packing.Maximum = 1;
	const core::Name packedMesh("object-id-packed");
	REQUIRE(renderer.AddPackedMesh(packedMesh, render::BuildPackedMeshData(editable)));
	centreRear.Mesh = packedMesh;
	scene::DrawInstance packed = labelled;
	packed.Source = 3;
	packed.Frame.Position = {1, 0, -4};
	packed.Mesh = packedMesh;
	packed.ObjectLabel = 2;
	packed.SemanticLabel = 1;
	packed.PartLabel = 2;
	scene::DrawInstance packedFront = packed;
	packedFront.Source = 6;
	packedFront.Frame.Position = {1, 0, -3};
	packedFront.PackedPbrMap = core::Name("object-id-orm");
	packedFront.RoughnessChannel = 2;
	packedFront.OcclusionChannel = 1;
	packedFront.MetalnessChannel = 0;
	// Height remains absent: ORM alpha must not make this plane displace.
	packedFront.HeightChannel = 255;
	const std::array rows{labelled, transparentFront, nativeRear, unlabelled, centreRear, packedFront};
	view.Instances = rows;
	render::DataCaptureRequest request{
		.SnapshotId = view.SnapshotId,
		.Pipeline = view.Pipeline,
		.CaptureNode = captureNode,
		.Channels =
			{
				render::DataCaptureChannel::RgbLinearHdr,
				render::DataCaptureChannel::LinearDepth,
				render::DataCaptureChannel::ShadingNormal,
				render::DataCaptureChannel::PbrAlbedo,
				render::DataCaptureChannel::MeshUv,
			},
		.ObjectLabels = {},
		.SemanticLabels = {},
		.PartLabels = {},
		.LocalLightIds = {},
	};
	render::DataCaptureTicket ticket;
	REQUIRE(renderer.QueueDataCapture(request, ticket));
	CHECK(ticket.ResourceTokens.size() == 3);
	CHECK(ticket.ChannelResourceIndices == std::vector<uint8_t>{0, 0, 0, 1, 2});
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(captureNode));

	render::DataCapturePoll captured;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		captured = renderer.PollDataCapture(ticket);
		if (captured.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (captured.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	REQUIRE(captured.Status == render::DataCaptureStatus::Ready);
	REQUIRE(captured.SnapshotId == view.SnapshotId);
	REQUIRE(captured.Planes.size() == 5);
	for (const auto &plane : captured.Planes) {
		REQUIRE(plane.Status == render::DataCaptureStatus::Ready);
		CHECK(plane.Width == target.Width);
		CHECK(plane.Height == target.Height);
		CHECK(plane.Hash == assets::Hasher::Of(plane.Bytes));
	}
	CHECK(captured.Planes[0].Channel == render::DataCaptureChannel::RgbLinearHdr);
	CHECK(captured.Planes[1].Channel == render::DataCaptureChannel::LinearDepth);
	CHECK(captured.Planes[2].Channel == render::DataCaptureChannel::ShadingNormal);
	CHECK(captured.Planes[3].Channel == render::DataCaptureChannel::PbrAlbedo);
	CHECK(captured.Planes[3].Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(captured.Planes[3].ColourSpace == render::DataCaptureColourSpace::SRGB);
	CHECK(captured.Planes[4].Channel == render::DataCaptureChannel::MeshUv);
	CHECK(captured.Planes[4].Scalar == render::DataCaptureScalar::Float16);
	CHECK(captured.Planes[4].ColourSpace == render::DataCaptureColourSpace::NotApplicable);
	CHECK(captured.Planes[4].RowStride == captured.Planes[4].Width * 4);
	// UV is stored as two IEEE 754 half values. Visible built-in fragments retain
	// the perspective-correct mesh interpolation while cleared background remains NaN.
	size_t finiteUvPixels = 0;
	size_t nanUvPixels = 0;
	for (size_t pixel = 0; pixel < captured.Planes[4].Width * captured.Planes[4].Height; ++pixel) {
		uint32_t packed = 0;
		std::memcpy(&packed, captured.Planes[4].Bytes.data() + pixel * 4, sizeof(packed));
		const glm::vec2 uv = glm::unpackHalf2x16(packed);
		if (std::isfinite(uv.x) && std::isfinite(uv.y)) {
			++finiteUvPixels;
		} else {
			++nanUvPixels;
			CHECK(std::isnan(uv.x));
			CHECK(std::isnan(uv.y));
		}
	}
	CHECK(finiteUvPixels > 0);
	CHECK(nanUvPixels > 0);
	CHECK(
		captured.Planes[4].Provenance ==
		"authored_mesh_texcoord/"
		"v1;components=u_v;units=dimensionless;range=unbounded;"
		"interpolation=perspective_correct;surface=visible_builtin_opaque_or_masked;"
		"validity=both_float16_components_finite"
	);
	CHECK(captured.CameraPose.NearPlaneMetres == view.Camera.NearPlane);
	CHECK(captured.CameraPose.FarPlaneMetres == view.Camera.FarPlane);

	request.Channels = {
		render::DataCaptureChannel::RgbLinearHdr,
		render::DataCaptureChannel::LinearDepth,
		render::DataCaptureChannel::ShadingNormal,
		render::DataCaptureChannel::PbrAlbedo,
		render::DataCaptureChannel::PbrMaterial,
		render::DataCaptureChannel::PbrEmissive,
		render::DataCaptureChannel::AmbientOcclusion,
		render::DataCaptureChannel::ObjectIds,
		render::DataCaptureChannel::SemanticMask,
		render::DataCaptureChannel::PartMask,
		render::DataCaptureChannel::SecondSurfaceDepth,
		render::DataCaptureChannel::SecondSurfaceValidity,
		render::DataCaptureChannel::PbrSpecular,
		render::DataCaptureChannel::PbrTransmission,
	};
	request.ObjectLabels = {{1, "fixture/alpha"}, {2, "fixture/packed"}};
	request.SemanticLabels = {{1, "fixture/box"}};
	request.PartLabels = {{1, "fixture/alpha"}, {2, "fixture/packed"}};
	render::DataCaptureTicket partial;
	REQUIRE(renderer.QueueDataCapture(request, partial));
	CHECK(partial.ResourceTokens.size() == 11);
	CHECK(partial.ChannelResourceIndices.size() == request.Channels.size());
	CHECK(partial.ChannelResourceIndices[10] == partial.ChannelResourceIndices[11]);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(captureNode));
	do {
		captured = renderer.PollDataCapture(partial);
		if (captured.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (captured.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	REQUIRE(captured.Status == render::DataCaptureStatus::Ready);
	CHECK(captured.Planes[0].Status == render::DataCaptureStatus::Ready);
	REQUIRE(captured.Planes.size() == 14);
	for (const auto &plane : captured.Planes) {
		REQUIRE(plane.Status == render::DataCaptureStatus::Ready);
		CHECK(plane.Hash == assets::Hasher::Of(plane.Bytes));
		if (plane.Channel != render::DataCaptureChannel::AmbientOcclusion) {
			CHECK(plane.Width == target.Width);
			CHECK(plane.Height == target.Height);
		}
	}
	CHECK_FALSE(captured.Planes[0].Bytes.empty());
	CHECK_FALSE(captured.Planes[1].Bytes.empty());
	CHECK_FALSE(captured.Planes[2].Bytes.empty());
	CHECK(captured.Planes[6].Channel == render::DataCaptureChannel::AmbientOcclusion);
	CHECK(captured.Planes[6].Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(captured.Planes[6].ColourSpace == render::DataCaptureColourSpace::NotApplicable);
	CHECK(captured.Planes[6].Origin == render::DataCaptureOrigin::TopLeft);
	CHECK(captured.Planes[6].Width == target.Width / 2);
	CHECK(captured.Planes[6].Height == target.Height / 2);
	CHECK(captured.Planes[6].RowStride == captured.Planes[6].Width);
	REQUIRE(captured.Planes[6].Bytes.size() == captured.Planes[6].Width * captured.Planes[6].Height);
	for (size_t planeIndex = 7; planeIndex < 10; ++planeIndex) {
		CHECK(captured.Planes[planeIndex].Status == render::DataCaptureStatus::Ready);
		CHECK(captured.Planes[planeIndex].Scalar == render::DataCaptureScalar::UInt32);
		CHECK(captured.Planes[planeIndex].RowStride == captured.Planes[planeIndex].Width * 4);
		REQUIRE(
			captured.Planes[planeIndex].Bytes.size() ==
			captured.Planes[planeIndex].Width * captured.Planes[planeIndex].Height * 4
		);
	}
	REQUIRE(captured.SemanticLabels.size() == 1);
	REQUIRE(captured.PartLabels.size() == 2);
	CHECK(captured.Planes[1].Scalar == render::DataCaptureScalar::Float32);
	CHECK(captured.Planes[2].Scalar == render::DataCaptureScalar::UNorm10A2);
	CHECK(captured.Planes[3].Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(captured.Planes[3].ColourSpace == render::DataCaptureColourSpace::SRGB);
	CHECK(captured.Planes[4].Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(captured.Planes[4].ColourSpace == render::DataCaptureColourSpace::NotApplicable);
	CHECK(captured.Planes[5].Scalar == render::DataCaptureScalar::Float16);
	CHECK(captured.Planes[5].ColourSpace == render::DataCaptureColourSpace::Linear);
	CHECK(captured.Planes[12].Channel == render::DataCaptureChannel::PbrSpecular);
	CHECK(captured.Planes[12].Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(
		captured.Planes[12].Provenance ==
		"authored_specular_factor/v1;source=material_alpha;range=zero_to_one"
	);
	CHECK(captured.Planes[13].Channel == render::DataCaptureChannel::PbrTransmission);
	CHECK(captured.Planes[13].Scalar == render::DataCaptureScalar::Float16);
	CHECK(
		captured.Planes[13].Provenance ==
		"authored_transmission_factor/v2;source=emissive_alpha;range=zero_to_one;"
		"visual_model=authored_ior_thickness_guard_refraction"
	);
	core::ByteReader objectIds(captured.Planes[7].Bytes);
	size_t labelledPixels = 0, packedPixels = 0, zeroPixels = 0;
	uint32_t centreLabel = 99;
	for (size_t pixel = 0; pixel < captured.Planes[7].Width * captured.Planes[7].Height; ++pixel) {
		const uint32_t label = objectIds.ReadUInt32();
		if (label == 1) ++labelledPixels;
		if (label == 2) {
			++packedPixels;
			const size_t material = pixel * 4;
			CHECK(std::to_integer<uint8_t>(captured.Planes[4].Bytes[material]) == 192);
			CHECK(std::to_integer<uint8_t>(captured.Planes[4].Bytes[material + 1]) == 64);
			CHECK(std::to_integer<uint8_t>(captured.Planes[4].Bytes[material + 2]) == 128);
			CHECK(std::to_integer<uint8_t>(captured.Planes[4].Bytes[material + 3]) == 255);
			CHECK(std::to_integer<uint8_t>(captured.Planes[12].Bytes[pixel]) == 255);
			CHECK(captured.Planes[13].Bytes[pixel * 2] == std::byte{0});
			CHECK(captured.Planes[13].Bytes[pixel * 2 + 1] == std::byte{0});
		}
		if (label == 0) ++zeroPixels;
		if (pixel ==
			(captured.Planes[1].Height / 2) * captured.Planes[1].Width + captured.Planes[1].Width / 2)
			centreLabel = label;
		CHECK((label == 0 || label == 1 || label == 2));
	}
	CHECK(labelledPixels > 0);
	CHECK(packedPixels > 0);
	CHECK(zeroPixels > 0);
	CHECK(centreLabel == 0);
	REQUIRE(captured.ObjectLabels.size() == 2);
	CHECK(captured.ObjectLabels[0].Label == 1);
	CHECK(captured.ObjectLabels[0].StableId == "fixture/alpha");
	CHECK(captured.ObjectLabels[1].Label == 2);
	CHECK(captured.ObjectLabels[1].StableId == "fixture/packed");
	const auto checkIdPlane = [&](size_t planeIndex, std::initializer_list<uint32_t> allowed) {
		core::ByteReader ids(captured.Planes[planeIndex].Bytes);
		size_t background = 0;
		std::array<size_t, 3> seen{};
		for (size_t pixel = 0; pixel < captured.Planes[planeIndex].Width * captured.Planes[planeIndex].Height;
			 ++pixel) {
			const uint32_t label = ids.ReadUInt32();
			CHECK(std::find(allowed.begin(), allowed.end(), label) != allowed.end());
			if (label == 0) ++background;
			if (label < seen.size()) ++seen[label];
		}
		CHECK(background > 0);
		for (const uint32_t label : allowed)
			if (label != 0) CHECK(seen[label] > 0);
	};
	checkIdPlane(8, {0, 1});
	checkIdPlane(9, {0, 1, 2});

	const auto &secondDepth = captured.Planes[10];
	const auto &secondValidity = captured.Planes[11];
	CHECK(secondDepth.Channel == render::DataCaptureChannel::SecondSurfaceDepth);
	CHECK(secondDepth.Scalar == render::DataCaptureScalar::Float32);
	CHECK(secondValidity.Channel == render::DataCaptureChannel::SecondSurfaceValidity);
	CHECK(secondValidity.Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(secondDepth.Width == captured.Planes[1].Width);
	CHECK(secondDepth.Height == captured.Planes[1].Height);
	CHECK(secondValidity.Width == secondDepth.Width);
	CHECK(secondValidity.Height == secondDepth.Height);
	CHECK_FALSE(secondDepth.Provenance.empty());
	CHECK(secondValidity.Provenance == secondDepth.Provenance);
	core::ByteReader visibleDepth(captured.Planes[1].Bytes), hiddenDepth(secondDepth.Bytes);
	size_t validHiddenPixels = 0;
	size_t invalidHiddenPixels = 0;
	size_t maskedOpaquePixels = 0;
	size_t maskedHolePixels = 0;
	size_t nativeHiddenPixels = 0;
	size_t transparentDepthPixels = 0;
	for (size_t pixel = 0; pixel < secondDepth.Width * secondDepth.Height; ++pixel) {
		const float visibleMetres = visibleDepth.ReadFloat();
		const float hiddenMetres = hiddenDepth.ReadFloat();
		const uint8_t validity = std::to_integer<uint8_t>(secondValidity.Bytes[pixel]);
		CHECK((validity == 0 || validity == 255));
		if (validity == 0) {
			++invalidHiddenPixels;
			CHECK(hiddenMetres == 0.f);
		} else {
			++validHiddenPixels;
			CHECK(hiddenMetres > visibleMetres);
			CHECK(visibleMetres > 0.f);
		}
		if (visibleMetres == Catch::Approx(3.5f).margin(.05f) &&
			hiddenMetres == Catch::Approx(4.5f).margin(.05f))
			++maskedOpaquePixels;
		if (visibleMetres == Catch::Approx(4.5f).margin(.05f) && validity == 0) ++maskedHolePixels;
		if (validity == 255 && hiddenMetres == Catch::Approx(6.f).margin(.05f)) ++nativeHiddenPixels;
		if (hiddenMetres == Catch::Approx(5.f).margin(.05f)) ++transparentDepthPixels;
	}
	CHECK(validHiddenPixels > 0);
	CHECK(invalidHiddenPixels > 0);
	// The hidden centre plane is packed, so these pixels exercise packed decoding
	// through the same alpha-cutout eligibility path as the visible G-buffer.
	CHECK(maskedOpaquePixels > 0);
	CHECK(maskedHolePixels > 0);
	CHECK(nativeHiddenPixels > 0);
	CHECK(transparentDepthPixels == 0);

	render::DataCaptureTicket cancelled;
	REQUIRE(renderer.QueueDataCapture(request, cancelled));
	renderer.CancelDataCapture(cancelled);
	CHECK(cancelled.ResourceTokens.empty());
	CHECK(cancelled.ChannelResourceIndices.empty());
	CHECK(renderer.PollDataCapture(cancelled).Status == render::DataCaptureStatus::Cancelled);

	request.SnapshotId = "snapshot-render-2";
	request.Channels = {render::DataCaptureChannel::RgbLinearHdr};
	REQUIRE(renderer.QueueDataCapture(request, ticket));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(captureNode));
	do {
		captured = renderer.PollDataCapture(ticket);
		if (captured.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (captured.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	CHECK(captured.Status == render::DataCaptureStatus::Invalid);
}

TEST_CASE(
	"mesh UV capture records perspective interpolation and omitted fragments",
	"[render][gpu][data-capture][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), pipeline, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("mesh-uv-device-evidence");
	const core::Name captureNode("data-capture");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	assets::MeshData triangle;
	triangle.Vertices = {
		{{-.4f, -.4f, 0}, {0, 0, 1}, {0, 0}},
		{{.6f, -.3f, 0}, {0, 0, 1}, {1, 0}},
		{{-.1f, .6f, 0}, {0, 0, 1}, {0, 1}},
	};
	triangle.Indices = {0, 1, 2};
	triangle.ComputeBounds();
	const core::Name triangleName("mesh-uv-perspective-triangle");
	REQUIRE(renderer.AddMesh(triangleName, triangle));

	assets::TextureData transparent;
	transparent.Width = transparent.Height = 1;
	transparent.Format = assets::TextureFormat::RGBA8;
	transparent.Pixels = {std::byte{255}, std::byte{255}, std::byte{255}, std::byte{0}};
	const core::Name transparentName("mesh-uv-discard");
	REQUIRE(renderer.AddTexture(transparentName, transparent));

	render::ShaderCompiler compiler;
	const auto customProgram = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(0,1,0,1);}\n",
		render::ShaderStage::Fragment,
		"mesh-uv-omitted.frag"
	);
	INFO(customProgram.Error);
	REQUIRE_FALSE(customProgram.Failed);
	const core::Name customShader("mesh-uv-omitted-custom-shader");
	REQUIRE(renderer.AddShader(customShader, customProgram.SpirV));

	constexpr uint32_t WIDTH = 64;
	constexpr uint32_t HEIGHT = 64;
	constexpr uint32_t PIXEL_X = 30;
	constexpr uint32_t PIXEL_Y = 30;
	render::SceneTarget target{WIDTH, HEIGHT};
	render::View view;
	view.Pipeline = pipelineName;
	view.Target = &target;
	glm::mat4 projection(0.0f);
	projection[0][0] = 1.0f;
	projection[1][1] = 1.0f;
	projection[2][2] = 1.0f;
	projection[0][3] = .5f;
	projection[3][2] = .5f;
	projection[3][3] = 1.0f;
	view.Projection = projection;

	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Mesh = triangleName;
	// Keep the mesh's authored local coordinates after residency centres it.
	// The expected interpolation below is therefore over these exact vertices.
	instance.Frame.Position = {.1f, .1f, 0};
	instance.HalfExtent = {.5f, .5f, .5f};
	instance.CastShadow = false;

	const auto capture =
		[&](scene::DrawInstance row,
			std::string snapshot,
			std::vector<render::DataCaptureChannel> channels = {render::DataCaptureChannel::MeshUv}) {
			view.SnapshotId = std::move(snapshot);
			view.Instances = std::span(&row, 1);
			render::DataCaptureRequest request{
				.SnapshotId = view.SnapshotId,
				.Pipeline = pipelineName,
				.CaptureNode = captureNode,
				.Channels = std::move(channels),
				.ObjectLabels = {},
				.SemanticLabels = {},
				.PartLabels = {},
				.LocalLightIds = {},
			};
			render::DataCaptureTicket ticket;
			REQUIRE(renderer.QueueDataCapture(request, ticket));
			render::OverlayImage overlay;
			REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(captureNode));
			render::DataCapturePoll poll;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do {
				poll = renderer.PollDataCapture(ticket);
				if (poll.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
			} while (poll.Status == render::DataCaptureStatus::Pending &&
					 std::chrono::steady_clock::now() < deadline);
			REQUIRE(poll.Status == render::DataCaptureStatus::Ready);
			return poll;
		};
	const auto plane = [](const render::DataCapturePoll &poll, render::DataCaptureChannel channel) {
		const auto found =
			std::find_if(poll.Planes.begin(), poll.Planes.end(), [channel](const auto &candidate) {
				return candidate.Channel == channel;
			});
		return found == poll.Planes.end() ? nullptr : &*found;
	};

	const auto at = [](const render::DataCapturePlane &plane, uint32_t x, uint32_t y) {
		REQUIRE(x < plane.Width);
		REQUIRE(y < plane.Height);
		uint32_t packed = 0;
		std::memcpy(&packed, plane.Bytes.data() + y * plane.RowStride + x * 4, sizeof(packed));
		return glm::unpackHalf2x16(packed);
	};
	const auto rgbaAt = [](const render::DataCapturePlane &plane, uint32_t x, uint32_t y) {
		REQUIRE(x < plane.Width);
		REQUIRE(y < plane.Height);
		uint32_t lower = 0;
		uint32_t upper = 0;
		const std::byte *const pixel = plane.Bytes.data() + y * plane.RowStride + x * 8;
		std::memcpy(&lower, pixel, sizeof(lower));
		std::memcpy(&upper, pixel + sizeof(lower), sizeof(upper));
		const glm::vec2 redGreen = glm::unpackHalf2x16(lower);
		const glm::vec2 blueAlpha = glm::unpackHalf2x16(upper);
		return glm::vec4{redGreen, blueAlpha};
	};

	const render::DataCapturePoll visiblePoll = capture(instance, "mesh-uv-visible");
	REQUIRE(visiblePoll.Planes.size() == 1);
	const render::DataCapturePlane *const visible = plane(visiblePoll, render::DataCaptureChannel::MeshUv);
	REQUIRE(visible != nullptr);
	const glm::vec2 observed = at(*visible, PIXEL_X, PIXEL_Y);
	const auto screen = [](const glm::vec4 &clip) {
		return glm::vec2{(clip.x / clip.w * .5f + .5f) * WIDTH, (clip.y / clip.w * .5f + .5f) * HEIGHT};
	};
	const std::array clip = {
		projection * glm::vec4(triangle.Vertices[0].Position[0], triangle.Vertices[0].Position[1], 0, 1),
		projection * glm::vec4(triangle.Vertices[1].Position[0], triangle.Vertices[1].Position[1], 0, 1),
		projection * glm::vec4(triangle.Vertices[2].Position[0], triangle.Vertices[2].Position[1], 0, 1),
	};
	const std::array screenPoints = {screen(clip[0]), screen(clip[1]), screen(clip[2])};
	const glm::vec2 point{PIXEL_X + .5f, HEIGHT - PIXEL_Y - .5f};
	const auto edge = [](glm::vec2 first, glm::vec2 second, glm::vec2 sample) {
		return (second.x - first.x) * (sample.y - first.y) - (second.y - first.y) * (sample.x - first.x);
	};
	const float area = edge(screenPoints[0], screenPoints[1], screenPoints[2]);
	REQUIRE(std::abs(area) > .001f);
	const float first = edge(screenPoints[1], screenPoints[2], point) / area;
	const float second = edge(screenPoints[2], screenPoints[0], point) / area;
	const float third = edge(screenPoints[0], screenPoints[1], point) / area;
	REQUIRE((first > 0 && second > 0 && third > 0));
	const float reciprocalW = first / clip[0].w + second / clip[1].w + third / clip[2].w;
	const glm::vec2 expected = (first * glm::vec2{0, 0} / clip[0].w + second * glm::vec2{1, 0} / clip[1].w +
								third * glm::vec2{0, 1} / clip[2].w) /
							   reciprocalW;
	CHECK(std::isfinite(observed.x));
	CHECK(std::isfinite(observed.y));
	CHECK(observed.x == Catch::Approx(expected.x).margin(.002f));
	CHECK(observed.y == Catch::Approx(expected.y).margin(.002f));
	const glm::vec2 background = at(*visible, 0, 0);
	CHECK(std::isnan(background.x));
	CHECK(std::isnan(background.y));

	scene::DrawInstance masked = instance;
	masked.Texture = transparentName;
	masked.Alpha = scene::AlphaMode::Transparency;
	masked.AlphaCutoff = .5f;
	const render::DataCapturePoll maskedPoll = capture(masked, "mesh-uv-masked");
	const render::DataCapturePlane *const maskedUv = plane(maskedPoll, render::DataCaptureChannel::MeshUv);
	REQUIRE(maskedUv != nullptr);
	const glm::vec2 discarded = at(*maskedUv, PIXEL_X, PIXEL_Y);
	CHECK(std::isnan(discarded.x));
	CHECK(std::isnan(discarded.y));

	scene::DrawInstance custom = instance;
	custom.Shader = customShader;
	const render::DataCapturePoll customPoll = capture(
		custom,
		"mesh-uv-custom",
		{render::DataCaptureChannel::RgbLinearHdr, render::DataCaptureChannel::MeshUv}
	);
	const render::DataCapturePlane *const customColour =
		plane(customPoll, render::DataCaptureChannel::RgbLinearHdr);
	const render::DataCapturePlane *const customUv = plane(customPoll, render::DataCaptureChannel::MeshUv);
	REQUIRE(customColour != nullptr);
	REQUIRE(customUv != nullptr);
	const glm::vec4 customPixel = rgbaAt(*customColour, PIXEL_X, PIXEL_Y);
	CHECK(std::isfinite(customPixel.r));
	CHECK(std::isfinite(customPixel.g));
	CHECK(std::isfinite(customPixel.b));
	CHECK(customPixel.r == Catch::Approx(0.0f).margin(.01f));
	CHECK(customPixel.g == Catch::Approx(1.0f).margin(.01f));
	CHECK(customPixel.b == Catch::Approx(0.0f).margin(.01f));
	const glm::vec2 omitted = at(*customUv, PIXEL_X, PIXEL_Y);
	CHECK(std::isnan(omitted.x));
	CHECK(std::isnan(omitted.y));
}
