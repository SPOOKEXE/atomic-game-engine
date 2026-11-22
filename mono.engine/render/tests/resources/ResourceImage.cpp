// Resource image requests, resident adoption, export ownership, reuse, eviction,
// capture groups, token cancellation and ambient occlusion capture.
#include "ResourceImageFixture.hpp"

TEST_SUITE_ID("engine.render.resourceimage")

TEST_CASE(
	"an unavailable eight-target G-buffer selects the forward default", "[render][gpu][capabilities][.]"
) {
	ForcedGBufferFailure forced;
	render::test::FixtureDevice fixture;
	fixture.Initialise();

	const render::DeviceCaps &caps = fixture.Render.Capabilities();
	CHECK(caps.MaxColourTargets == 0);
	const render::PipelineTierDecision selected = render::ChooseDefaultPipeline(caps);
	CHECK(selected.Tier == render::DefaultPipelineTier::C);
	REQUIRE(selected.Fallthrough.size() == 2);
	CHECK(selected.Fallthrough[0].Cause.Status == render::CapabilityStatus::InsufficientColourTargets);
	CHECK(selected.Fallthrough[1].Cause.Status == render::CapabilityStatus::InsufficientColourTargets);

	const auto defaultGraph = fixture.Render.DescribePipeline(core::Name("Engine Default"), 32, 24);
	REQUIRE(defaultGraph);
	bool hasForward = false;
	bool hasGBuffer = false;
	for (uint32_t index = 1; index <= defaultGraph->Graph.Count(); ++index) {
		const graph::Node *node = defaultGraph->Graph.Find(graph::NodeId{index});
		REQUIRE(node != nullptr);
		hasForward = hasForward || node->Kind == core::Name("forward");
		hasGBuffer = hasGBuffer || node->Kind == core::Name("gbuffer");
	}
	CHECK(hasForward);
	CHECK_FALSE(hasGBuffer);
}

TEST_CASE(
	"a successful eight-target G-buffer probe reports the PBR capability", "[render][gpu][capabilities][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();

	const render::DeviceCaps &caps = fixture.Render.Capabilities();
	REQUIRE(caps.MaxColourTargets == 8);
	CHECK(render::ChooseDefaultPipeline(caps).Tier == render::DefaultPipelineTier::A);

	const auto defaultGraph = fixture.Render.DescribePipeline(core::Name("Engine Default"), 32, 24);
	REQUIRE(defaultGraph);
	bool hasGBuffer = false;
	for (uint32_t index = 1; index <= defaultGraph->Graph.Count(); ++index) {
		const graph::Node *node = defaultGraph->Graph.Find(graph::NodeId{index});
		REQUIRE(node != nullptr);
		hasGBuffer = hasGBuffer || node->Kind == core::Name("gbuffer");
	}
	CHECK(hasGBuffer);
}

TEST_CASE("resource image requests require a live device", "[render][resourceimage]") {
	engine::render::Renderer renderer;
	CHECK_FALSE(renderer.RequestResourceImage({.Token = 1, .Node = engine::core::Name("capture")}));
	CHECK(renderer.TakeResourceImages().empty());
	CHECK_FALSE(renderer.CancelResourceImage(1));
	CHECK_FALSE(renderer.TakeResourceImage(1));
	CHECK(renderer.QueueResourceImage({}, engine::core::Name("capture")) == 0);
}

TEST_CASE(
	"resident graph image adoption transfers ownership without readback", "[render][gpu][resourceimage][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const int planeSet = GENERATE(0, 1, 2, 3);
	const bool pairedDepth = planeSet != 0;
	const bool retainedAmbient = planeSet >= 2;
	const bool retainedDirectional = planeSet == 3;
	const bool rotatedCamera = GENERATE(false, true);
	CAPTURE(planeSet, rotatedCamera);
	InstallImageCapture(renderer, "lit", pairedDepth, retainedAmbient, retainedDirectional);
	const auto token = renderer.QueueResourceImage(
		core::Name("image-export-pipeline"),
		core::Name("image-export"),
		0,
		render::ResourceImageDelivery::Resident
	);
	REQUIRE(token != 0);
	render::PortalImageBinding binding;
	binding.WorldName = core::Name("resident-source");
	if (retainedAmbient) binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.Portal = core::Name("Door");
	binding.Expected.RequestId = 1;
	binding.Expected.PortalKey = "Door";
	CHECK(renderer.AdoptResourceImage(token, binding) == 0);
	CHECK_FALSE(renderer.CanPublishResourceImage(token, 65, 37));
	render::SceneTarget target{65, 37};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	if (rotatedCamera)
		view.CameraFrame =
			core::CFrame(core::Vector3{7, 2, 3}, core::CFrame::Angles(-.2f, .37f, .1f).Rotation());
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("resident-plane"), plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("resident-white"), white));
	scene::DrawInstance wall;
	wall.Source = 1;
	wall.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -4});
	wall.HalfExtent = {100, 100, .01f};
	wall.Mesh = core::Name("resident-plane");
	wall.Texture = core::Name("resident-white");
	wall.Tint = {.5f, .125f, .25f};
	wall.CastShadow = false;
	view.Instances = std::span(&wall, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {1, 1, 1};
	view.Lighting.OutdoorAmbient = {1, 1, 1};
	view.Lighting.Direct = {};
	const auto copiedToken =
		renderer.QueueResourceImage(core::Name("image-export-pipeline"), core::Name("image-export"));
	REQUIRE(copiedToken != 0);
	binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 65.0f / 37.0f).ViewProjection;
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	CHECK_FALSE(renderer.TakeResourceImage(token));
	CHECK_FALSE(renderer.TakeResourceImages(std::array{copiedToken, token}));
	CHECK(renderer.CanPublishResourceImage(token, 65, 37));
	CHECK_FALSE(renderer.CanPublishResourceImage(token, 37, 65));
	CHECK_FALSE(renderer.CanPublishResourceImage(copiedToken, 65, 37));
	auto invalid = binding;
	invalid.Expected.PortalKey = "Other";
	CHECK(renderer.AdoptResourceImage(token, invalid) == 0);
	const auto handle = renderer.AdoptResourceImage(token, binding);
	REQUIRE(handle != 0);
	CHECK_FALSE(renderer.CanPublishResourceImage(token, 65, 37));
	CHECK(renderer.AdoptResourceImage(token, binding) == 0);
	CHECK(renderer.PortalImageUsage().Images == 1);
	CHECK(
		renderer.PortalImageUsage().TextureBytes == 65 * 37 *
														(retainedDirectional ? 64
														 : retainedAmbient	 ? 48
														 : pairedDepth		 ? 12
																			 : 8)
	);
	CHECK(renderer.PortalImageUsage().UploadedBytes == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
	CHECK(renderer.PortalImageUsage().StagingBytes == 0);
	const auto copied = AwaitImage(renderer, copiedToken);
	if (retainedDirectional) {
		CHECK(copied.DirectionalResponseResource == core::Name("directional-response"));
		REQUIRE(copied.DirectionalResponse.size() == 65 * 37 * 16);
		core::ByteReader samples(copied.DirectionalResponse);
		while (!samples.AtEnd()) {
			CHECK(samples.ReadFloat() == 0);
			CHECK(samples.ReadFloat() == 0);
			CHECK(samples.ReadFloat() == 0);
			const float visibility = samples.ReadFloat();
			CHECK(visibility >= 0);
			CHECK(visibility <= 1);
		}
	} else
		CHECK(copied.DirectionalResponse.empty());
	REQUIRE(copied.Status == render::ResourceImageStatus::Ok);
	if (pairedDepth) {
		CHECK(copied.DepthResource == core::Name("linear-depth"));
		REQUIRE(copied.Depth.size() == 65 * 37 * 4);
		core::ByteReader depths(copied.Depth);
		float minimum = 1000, maximum = 0;
		while (!depths.AtEnd()) {
			const float distance = depths.ReadFloat();
			REQUIRE(std::isfinite(distance));
			minimum = std::min(minimum, distance);
			maximum = std::max(maximum, distance);
		}
		CHECK(std::abs(minimum - 4.f) < .01f);
		CHECK(std::abs(maximum - 4.f) < .01f);
	} else {
		CHECK_FALSE(copied.DepthResource.IsValid());
		CHECK(copied.Depth.empty());
	}
	if (retainedAmbient) {
		CHECK(copied.NormalResource == core::Name("normal"));
		CHECK(copied.AmbientResponseResource == core::Name("ambient-response"));
		REQUIRE(copied.Normal.size() == 65 * 37 * 4);
		REQUIRE(copied.AmbientResponse.size() == 65 * 37 * 16);
		CHECK(copied.LightingBaselineResource == core::Name("lighting-baseline"));
		REQUIRE(copied.LightingBaseline.size() == 65 * 37 * 16);
		core::ByteReader baseline(copied.LightingBaseline), colour(copied.Pixels);
		while (!baseline.AtEnd()) {
			const float first = baseline.ReadFloat(), second = baseline.ReadFloat();
			REQUIRE(std::isfinite(first));
			REQUIRE(std::isfinite(second));
			const uint32_t packed = colour.ReadUInt32();
			// The observed attachment conversion and CPU packing differ by one half
			// step. Bound storage conversion locally without changing image parity.
			for (size_t channel = 0; channel < 2; ++channel) {
				const uint32_t half = (packed >> (channel * 16)) & 0xffffu;
				const float value = channel == 0 ? first : second;
				REQUIRE(value >= 0);
				REQUIRE(half < 0x7c00u);
				const float lower = glm::unpackHalf2x16(half == 0 ? 0 : half - 1).x;
				const float upper = glm::unpackHalf2x16(half + 1).x;
				CHECK(value >= lower);
				CHECK(value <= upper);
			}
		}
		core::ByteReader normals(copied.Normal);
		const auto expectedNormal = wall.Frame.VectorToWorldSpace({0, 0, 1});
		while (!normals.AtEnd()) {
			const uint32_t packed = normals.ReadUInt32();
			const float normalX = float(packed & 1023u) / 1023.f * 2 - 1;
			const float normalY = float((packed >> 10) & 1023u) / 1023.f * 2 - 1;
			const float normalZ = float((packed >> 20) & 1023u) / 1023.f * 2 - 1;
			CHECK(std::abs(normalX - expectedNormal.X) < .003f);
			CHECK(std::abs(normalY - expectedNormal.Y) < .003f);
			CHECK(std::abs(normalZ - expectedNormal.Z) < .003f);
		}
		core::ByteReader response(copied.AmbientResponse);
		float maximum = 0;
		while (!response.AtEnd()) {
			const float value = response.ReadFloat();
			REQUIRE(std::isfinite(value));
			maximum = std::max(maximum, value);
		}
		CHECK(maximum > 0);
	} else {
		CHECK(copied.Normal.empty());
		CHECK(copied.AmbientResponse.empty());
		CHECK(copied.LightingBaseline.empty());
	}
	render::PortalView portal;
	portal.ExternalImage = true;
	portal.ImagePortal = binding.Portal;
	portal.ImportedImage = handle;
	portal.Centre = wall.Frame.Position;
	portal.Normal = wall.Frame.VectorToWorldSpace({0, 0, 1});
	portal.First = wall.Frame.VectorToWorldSpace({2, 0, 0});
	portal.Second = wall.Frame.VectorToWorldSpace({0, 2, 0});
	wall.HalfExtent = {2, 2, .01f};
	wall.Surface = 0;
	wall.Tint = {.01f, .01f, .01f};
	view.WorldName = binding.WorldName;
	view.Portals = std::span(&portal, 1);
	// Adopted residency survives replacing the source graph with a retained display capture.
	InstallImageCapture(renderer, "tonemapped");
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	const auto backendFormat = renderer.Backend().ColourFormat;
	const bool bgra = backendFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
					  backendFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
	const bool supportedFormat = bgra || backendFormat == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
								 backendFormat == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
	CAPTURE(backendFormat);
	REQUIRE(supportedFormat);
	const auto displayFormat =
		bgra ? render::test::ImageFormat::Bgra8Unorm : render::test::ImageFormat::Rgba8Unorm;
	const auto residentPixels =
		render::test::CaptureResource(renderer, core::Name("tonemapped"), 0, 65, 37, displayFormat);
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Scope = binding.ExpectedScope;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = copied.Width;
	reply.Height = copied.Height;
	reply.RowStride = copied.RowStride;
	reply.Pixels = copied.Pixels;
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	if (retainedAmbient) {
		reply.CaptureLighting.emplace();
		reply.Normal = copied.Normal;
		reply.AmbientResponse = copied.AmbientResponse;
		reply.LightingBaseline = copied.LightingBaseline;
		reply.NormalHash = assets::Hasher::Of(reply.Normal);
		reply.AmbientResponseHash = assets::Hasher::Of(reply.AmbientResponse);
		reply.LightingBaselineHash = assets::Hasher::Of(reply.LightingBaseline);
		if (retainedDirectional) {
			reply.DirectionalResponse = copied.DirectionalResponse;
			reply.DirectionalResponseHash = assets::Hasher::Of(reply.DirectionalResponse);
		}
	}
	if (pairedDepth) {
		reply.Depth = copied.Depth;
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
	}
	portal.ImportedImage = renderer.QueuePortalImage(binding, std::move(reply));
	REQUIRE(portal.ImportedImage != 0);
	CHECK(
		renderer.PortalImageUsage().TextureBytes == 65 * 37 *
														(retainedDirectional ? 64
														 : retainedAmbient	 ? 48
														 : pairedDepth		 ? 12
																			 : 8)
	);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	const auto copiedPixels =
		render::test::CaptureResource(renderer, core::Name("tonemapped"), 0, 65, 37, displayFormat);
	CHECK(residentPixels.Bytes == copiedPixels.Bytes);
	const size_t centre = 18 * residentPixels.RowStrideBytes + 32 * 4;
	REQUIRE(residentPixels.Bytes.size() > centre + 3);
	const auto *pixel = residentPixels.Bytes.data() + centre;
	const double red = render::test::image_comparison_detail::Sample(pixel, displayFormat, 0);
	const double green = render::test::image_comparison_detail::Sample(pixel, displayFormat, 1);
	const double blue = render::test::image_comparison_detail::Sample(pixel, displayFormat, 2);
	CHECK(red > blue);
	CHECK(blue > green);
	if (pairedDepth) {
		render::PortalImageReply colorOnly;
		colorOnly.Key = binding.Expected;
		colorOnly.Scope = binding.ExpectedScope;
		colorOnly.Status = render::PortalImageStatus::Ok;
		colorOnly.Width = copied.Width;
		colorOnly.Height = copied.Height;
		colorOnly.RowStride = copied.RowStride;
		colorOnly.Pixels = copied.Pixels;
		colorOnly.PixelHash = assets::Hasher::Of(colorOnly.Pixels);
		auto repeatedColor = colorOnly;
		portal.ImportedImage = renderer.QueuePortalImage(binding, std::move(colorOnly));
		REQUIRE(portal.ImportedImage != 0);
		CHECK(renderer.QueuePortalImage(binding, std::move(repeatedColor)) == portal.ImportedImage);
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(renderer.PortalImageUsage().TextureBytes == 65 * 37 * 8);
	}
	REQUIRE(renderer.DropPortalImage(portal.ImportedImage));
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK_FALSE(renderer.DropPortalImage(handle));
}

TEST_CASE("failed graphs cannot hand out resident portal images", "[render][gpu][resourceimage][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallFailedImageCapture(renderer);
	const auto token = renderer.QueueResourceImage(
		core::Name("image-export-pipeline"),
		core::Name("image-export"),
		0,
		render::ResourceImageDelivery::Resident
	);
	REQUIRE(token != 0);
	render::SceneTarget target{16, 16};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	render::OverlayImage overlay;
	(void)renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	render::PortalImageBinding binding;
	binding.WorldName = core::Name("resident-source");
	binding.Portal = core::Name("Door");
	binding.Expected = {1, "Door", 1, 1};
	CHECK(renderer.AdoptResourceImage(token, binding) == 0);
	render::PortalResidentImages images(renderer);
	const world::PresentationAddress source{"resident-source", "replies", 1, 1};
	const world::PresentationAddress producer{"destination", "requests", 1, 2};
	render::PortalImageRequest request;
	request.Key = binding.Expected;
	request.Width = request.Height = 16;
	request.PixelBudget = 256;
	const render::PortalResidentImages::Time now{};
	REQUIRE(images.Reserve(source, producer, request, binding, now));
	const render::PortalResidentReceipt receipt{request.Key, request.Scope, 0, 0, 0, 16, 16};
	CHECK_FALSE(images.Publish(source, producer, receipt, token, now));
	images.Clear();
	CHECK(renderer.PortalImageUsage().Images == 0);
	REQUIRE(renderer.CancelResourceImage(token));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool reused = false;
	while (!reused && std::chrono::steady_clock::now() < deadline) {
		(void)renderer.TakeResourceImages();
		reused = renderer.RequestResourceImage(
			{token,
			 core::Name("image-export-pipeline"),
			 core::Name("image-export"),
			 0,
			 render::ResourceImageDelivery::Resident}
		);
		if (!reused) {
			SDL_Delay(1);
		}
	}
	REQUIRE(reused);
	CHECK(renderer.CancelResourceImage(token));
}

TEST_CASE(
	"graph image export owns HDR pixels and bounds queued and completed work",
	"[render][gpu][resourceimage][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer);
	const auto request = [](uint64_t token) {
		return render::ResourceImageRequest{
			token, core::Name("image-export-pipeline"), core::Name("image-export"), 0
		};
	};
	CHECK_FALSE(renderer.RequestResourceImage(request(0)));
	// Generated groups skip explicit tokens and leave outputs untouched on refusal.
	REQUIRE(renderer.RequestResourceImage(request(1)));
	REQUIRE(renderer.RequestResourceImage(request(3)));
	const std::array groupNodes{core::Name("image-export"), core::Name("image-export")};
	std::array<uint64_t, 2> generatedGroup{777, 888};
	REQUIRE(renderer.QueueResourceImages(
		core::Name("image-export-pipeline"),
		groupNodes,
		0,
		render::ResourceImageDelivery::CopiedPixels,
		generatedGroup
	));
	CHECK(generatedGroup == std::array<uint64_t, 2>{2, 4});
	REQUIRE(renderer.RequestResourceImage(request(99)));
	for (uint64_t token = 200; token < 207; ++token)
		REQUIRE(renderer.RequestResourceImage(request(token)));
	std::array<uint64_t, 2> refusedGroup{777, 888};
	CHECK_FALSE(renderer.QueueResourceImages(
		core::Name("image-export-pipeline"),
		groupNodes,
		0,
		render::ResourceImageDelivery::CopiedPixels,
		refusedGroup
	));
	CHECK(refusedGroup == std::array<uint64_t, 2>{777, 888});
	for (uint64_t token = 1; token <= 4; ++token)
		REQUIRE(renderer.CancelResourceImage(token));
	REQUIRE(renderer.CancelResourceImage(99));
	for (uint64_t token = 200; token < 207; ++token)
		REQUIRE(renderer.CancelResourceImage(token));
	const std::array invalidNodes{core::Name("image-export"), core::Name("missing-capture")};
	CHECK_FALSE(renderer.QueueResourceImages(
		core::Name("image-export-pipeline"),
		invalidNodes,
		0,
		render::ResourceImageDelivery::CopiedPixels,
		refusedGroup
	));
	CHECK(refusedGroup == std::array<uint64_t, 2>{777, 888});
	const auto validGroup = std::array{request(100), request(101)};
	CHECK_FALSE(renderer.RequestResourceImages({}));
	CHECK_FALSE(renderer.RequestResourceImages(
		std::array{
			request(100),
			request(101),
			request(102),
			request(103),
			request(104),
			request(105),
			request(106),
			request(107),
			request(108),
			request(109),
			request(110),
			request(111),
			request(112)
		}
	));
	for (const int invalid : {0, 1, 2, 3, 4, 5}) {
		auto group = validGroup;
		switch (invalid) {
		case 0:
			group[1].Token = group[0].Token;
			break;
		case 1:
			group[1].Node = core::Name("missing-capture");
			break;
		case 2:
			group[1].Pipeline = core::Name("missing-pipeline");
			break;
		case 3:
			group[1].ViewSlot = 1;
			break;
		case 4:
			group[1].Delivery = render::ResourceImageDelivery::Resident;
			break;
		case 5:
			group[1].Token = 0;
			break;
		}
		CHECK_FALSE(renderer.RequestResourceImages(group));
		CHECK_FALSE(renderer.CancelResourceImage(100));
		CHECK_FALSE(renderer.CancelResourceImage(101));
	}

	auto absent = request(1);
	absent.Node = core::Name("absent");
	CHECK_FALSE(renderer.RequestResourceImage(absent));
	absent = request(1);
	absent.ViewSlot = 3;
	REQUIRE(renderer.RequestResourceImage(absent));
	REQUIRE(renderer.CancelResourceImage(absent.Token));
	REQUIRE(renderer.RequestResourceImage(request(99)));
	REQUIRE(renderer.RequestResourceImage(request(98)));
	for (uint64_t token = 1; token <= 10; token++) {
		REQUIRE(renderer.RequestResourceImage(request(token)));
	}
	CHECK_FALSE(renderer.RequestResourceImage(request(1)));
	CHECK_FALSE(renderer.RequestResourceImage(request(11)));
	REQUIRE(renderer.CancelResourceImage(4));
	CHECK_FALSE(renderer.RequestResourceImages(validGroup));
	CHECK_FALSE(renderer.CancelResourceImage(100));
	CHECK_FALSE(renderer.CancelResourceImage(101));
	CHECK_FALSE(renderer.RequestResourceImages(std::array{request(100), request(1)}));
	CHECK_FALSE(renderer.CancelResourceImage(100));

	REQUIRE(renderer.RequestResourceImage(request(11)));
	CHECK(renderer.TakeResourceImages().empty());

	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("export-plane"), plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("export-white"), white));
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame.Position = {0, 0, -4};
	instance.HalfExtent = {100, 100, 0.01f};
	instance.Tint = {1, 0, 0};
	instance.Mesh = core::Name("export-plane");
	instance.Texture = core::Name("export-white");
	instance.CastShadow = false;
	scene::WorldLighting lighting;
	lighting.Ambient = {2, 2, 2};
	lighting.OutdoorAmbient = {2, 2, 2};
	lighting.Direct = {};
	render::SceneTarget target{65, 37};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	view.Instances = std::span(&instance, 1);
	view.OverrideLighting = true;
	view.Lighting = lighting;
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	// Pending cancellation suppresses bytes while retaining staging ownership until completion.
	REQUIRE(renderer.CancelResourceImage(11));
	REQUIRE(renderer.CancelResourceImage(99));
	REQUIRE(renderer.CancelResourceImage(98));
	for (uint64_t token = 5; token <= 10; ++token)
		REQUIRE(renderer.CancelResourceImage(token));
	auto firstImage = AwaitImage(renderer, 1);
	CHECK_FALSE(renderer.TakeResourceImage(1));
	// A completed member must survive a group with an unfinished or invalid member.
	REQUIRE(renderer.RequestResourceImage(request(6)));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 6}));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 999999}));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 3}));
	CHECK_FALSE(renderer.TakeResourceImages(std::span<const uint64_t>{}));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 5>{3, 2, 1, 5, 6}));
	REQUIRE(renderer.CancelResourceImage(6));
	auto images = AwaitImageGroup(renderer, std::array<uint64_t, 2>{3, 2});
	CHECK(images[0].Request.Token == 3);
	CHECK(images[1].Request.Token == 2);
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 2}));
	images.push_back(std::move(firstImage));
	const std::array<std::byte, 8> red{
		std::byte{0},
		std::byte{0x40},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0x3c}
	};
	for (const auto &image : images) {
		CHECK(image.Request.Token <= 3);
		CHECK(image.Status == render::ResourceImageStatus::Ok);
		CHECK(image.Width == 65);
		CHECK(image.Height == 37);
		CHECK(image.RowStride == 65 * 8);
		REQUIRE(image.Pixels.size() == size_t(65 * 37 * 8));
		for (size_t pixel = 0; pixel < 65 * 37; pixel++) {
			REQUIRE(std::equal(red.begin(), red.end(), image.Pixels.begin() + pixel * 8));
		}
	}
	CHECK(renderer.TakeResourceImages().empty());
	const auto firstPixels = images.front().Pixels;
	REQUIRE(renderer.RequestResourceImage(request(6)));
	instance.Tint = {0, 1, 0};
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	auto green = AwaitImages(renderer, 1);
	CHECK(green.front().Pixels != firstPixels);
	CHECK(green.front().CaptureFrame > images.front().CaptureFrame);
	CHECK(images.front().Pixels == firstPixels);
	// The renderer's ordinary upload ring warms independently of the export pool.
	for (uint64_t token = 10; token < 14; token++) {
		REQUIRE(renderer.RequestResourceImage(request(token)));
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		(void)AwaitImages(renderer, 1);
	}
	const auto residency = renderer.MemoryStatistics();
	REQUIRE(renderer.RequestResourceImage(request(14)));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	(void)AwaitImages(renderer, 1);
	CHECK(renderer.MemoryStatistics().TransferBuffers == residency.TransferBuffers);
	CHECK(renderer.MemoryStatistics().TransferBufferBytes == residency.TransferBufferBytes);
	const uint64_t generated =
		renderer.QueueResourceImage(core::Name("image-export-pipeline"), core::Name("image-export"));
	REQUIRE(generated != 0);
	const uint64_t other =
		renderer.QueueResourceImage(core::Name("image-export-pipeline"), core::Name("image-export"));
	REQUIRE(other != 0);
	REQUIRE(other != generated);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	CHECK_FALSE(renderer.TakeResourceImage(999999));
	CHECK(AwaitImage(renderer, generated).Request.Token == generated);
	CHECK(AwaitImage(renderer, other).Request.Token == other);

	// A valid graph can expose a display target; the HDR export refuses that format explicitly.
	InstallImageCapture(renderer, "composed-image");
	REQUIRE(renderer.RequestResourceImage(request(7)));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	auto refused = AwaitImages(renderer, 1);
	CHECK(refused.front().Status == render::ResourceImageStatus::Unsupported);
	CHECK(refused.front().Pixels.empty());

	InstallFailedImageCapture(renderer);
	REQUIRE(renderer.RequestResourceImages(std::array{request(20), request(22)}));
	const auto failed = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(failed.Ran(core::Name("image-export")));
	auto failedImages = AwaitImageGroup(renderer, std::array<uint64_t, 2>{20, 22});
	CHECK(failedImages[1].Status == render::ResourceImageStatus::Failed);
	CHECK(failedImages.front().Status == render::ResourceImageStatus::Failed);
	CHECK(failedImages.front().Pixels.empty());
	CHECK(failedImages.front().Width == 0);
	// A failed frame must release queue capacity after its actual submission completes.
	InstallImageCapture(renderer);
	REQUIRE(renderer.RequestResourceImage(request(21)));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	CHECK(AwaitImages(renderer, 1).front().Status == render::ResourceImageStatus::Ok);
}

TEST_CASE(
	"an unpinned frame capture serves every requested view in one batch", "[render][gpu][resourceimage][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer);

	render::SceneTarget firstTarget{35, 19};
	render::SceneTarget secondTarget{65, 37};
	std::array<render::View, 2> views;
	views[0].Pipeline = core::Name("image-export-pipeline");
	views[0].Target = &firstTarget;
	views[0].Slot = 0;
	views[0].WorldName = core::Name("capture/first");
	views[0].SnapshotId = "capture-first";
	views[0].CameraFrame.Position = {1, 2, 3};
	views[1].Pipeline = views[0].Pipeline;
	views[1].Target = &secondTarget;
	views[1].Slot = 1;
	views[1].WorldName = core::Name("capture/second");
	views[1].SnapshotId = "capture-second";
	views[1].CameraFrame.Position = {4, 5, 6};

	const core::Name capture("image-export");
	const render::ResourceImageRequest first{
		.Token = 1,
		.Pipeline = views[0].Pipeline,
		.Node = capture,
		.ViewSlot = views[0].Slot,
		.ExpectedSnapshotId = views[0].SnapshotId,
	};
	const render::ResourceImageRequest second{
		.Token = 2,
		.Pipeline = views[1].Pipeline,
		.Node = capture,
		.ViewSlot = views[1].Slot,
		.ExpectedSnapshotId = views[1].SnapshotId,
	};
	REQUIRE(renderer.RequestResourceImage(first));
	REQUIRE(renderer.RequestResourceImage(second));

	render::OverlayImage overlay;
	const render::FrameResult frame = renderer.Render(views, overlay, nullptr, false);
	REQUIRE(frame.Submitted);
	REQUIRE(frame.Ran(capture));
	const auto images = AwaitImageGroup(renderer, std::array<uint64_t, 2>{first.Token, second.Token});
	const auto &firstImage = images[0];
	const auto &secondImage = images[1];
	CHECK(firstImage.Status == render::ResourceImageStatus::Ok);
	CHECK(secondImage.Status == render::ResourceImageStatus::Ok);
	CHECK(firstImage.Request.ViewSlot == views[0].Slot);
	CHECK(secondImage.Request.ViewSlot == views[1].Slot);
	CHECK(firstImage.Width == firstTarget.Width);
	CHECK(firstImage.Height == firstTarget.Height);
	CHECK(secondImage.Width == secondTarget.Width);
	CHECK(secondImage.Height == secondTarget.Height);
	CHECK(firstImage.SnapshotId == views[0].SnapshotId);
	CHECK(secondImage.SnapshotId == views[1].SnapshotId);
	CHECK(firstImage.CameraWorldFromCamera[12] == views[0].CameraFrame.Position.X);
	CHECK(secondImage.CameraWorldFromCamera[12] == views[1].CameraFrame.Position.X);
	CHECK(firstImage.CaptureFrame == secondImage.CaptureFrame);
	REQUIRE(firstImage.Observation);
	REQUIRE(secondImage.Observation);
	CHECK(firstImage.Observation->WorldName == views[0].WorldName);
	CHECK(secondImage.Observation->WorldName == views[1].WorldName);
}

TEST_CASE(
	"resident body exports reuse replaced image pairs", "[render][gpu][resourceimage][resident-reuse][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const int planeSet = GENERATE(0, 1, 2);
	const bool ambient = planeSet != 0;
	const bool directional = planeSet == 2;
	const size_t pixelBytes = directional ? 64 : ambient ? 48 : 12;
	CAPTURE(planeSet);
	InstallImageCapture(renderer, "lit", true, ambient, directional);
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("resident-reuse-emission");
	REQUIRE(renderer.AddTexture(emission, white));
	render::SceneTarget target{65, 37};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("resident-reuse");
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	view.Pipeline = core::Name("image-export-pipeline");
	scene::DrawInstance body;
	body.Source = 1;
	body.HalfExtent = {.5f, .5f, .5f};
	body.CastShadow = false;
	body.Tint = {};
	body.EmissiveMap = emission;
	body.EmissiveStrength = 1.0f;
	view.Instances = std::span(&body, 1);
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("current-body");
	binding.Expected.PortalKey = "current-body";
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	uint64_t imported = 0, warmAllocations = 0;
	for (uint64_t frame = 0; frame < 12; ++frame) {
		body.Frame.Position = {0, 0, frame % 2 ? -5.f : -3.f};
		body.EmissiveTint = frame % 2 ? core::Color3{0, 1, 0} : core::Color3{1, 0, 0};
		const auto token = renderer.QueueResourceImage(
			view.Pipeline, core::Name("image-export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(token != 0);
		// The copy shares the resident capture's fence. AwaitImage has a ten-second
		// deadline, so the next reuse sample starts after this submission completes.
		const auto completion = renderer.QueueResourceImage(view.Pipeline, core::Name("image-export"));
		REQUIRE(completion != 0);
		render::OverlayImage overlay;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		binding.Expected.RequestId = frame + 1;
		imported = renderer.AdoptResourceImage(token, binding);
		REQUIRE(imported != 0);
		REQUIRE(AwaitImage(renderer, completion).Status == render::ResourceImageStatus::Ok);
		CHECK(renderer.PortalImageUsage().Images == 1);
		CHECK(renderer.PortalImageUsage().Uploads == 0);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		const auto allocations = renderer.MemoryStatistics().TextureAllocations;
		if (frame > 0) CHECK(renderer.PortalImageUsage().CachedTextureBytes == size_t(65) * 37 * pixelBytes);
		if (frame == 3) warmAllocations = allocations;
		if (frame > 3) CHECK(allocations == warmAllocations);
	}
	// More shapes than cache slots exercise bounded eviction and exact extent matching.
	body.Frame.Position = {0, 0, -7};
	body.EmissiveTint = {0, 0, 1};
	for (const uint32_t width : {33u, 17u, 9u, 5u, 65u}) {
		target.Width = width;
		const auto token = renderer.QueueResourceImage(
			view.Pipeline, core::Name("image-export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(token != 0);
		// The copy shares the resident capture's fence. AwaitImage has a ten-second
		// deadline, so the next reuse sample starts after this submission completes.
		const auto completion = renderer.QueueResourceImage(view.Pipeline, core::Name("image-export"));
		REQUIRE(completion != 0);
		render::OverlayImage overlay;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		binding.Expected.RequestId++;
		imported = renderer.AdoptResourceImage(token, binding);
		REQUIRE(imported != 0);
		REQUIRE(AwaitImage(renderer, completion).Status == render::ResourceImageStatus::Ok);
		CHECK(renderer.PortalImageUsage().CachedTextureBytes <= size_t(4) * 65 * 37 * pixelBytes);
		CHECK(renderer.PortalImageUsage().Images == 1);
		CHECK(renderer.PortalImageUsage().Uploads == 0);
	}
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPortalBodyDocument(), graph, offender) == graph::PipelineDocumentStatus::Ok
	);
	view.Pipeline = core::Name("resident-body-read");
	REQUIRE(renderer.SetPipeline(view.Pipeline, graph));
	view.Instances = {};
	view.EyeImage = imported;
	view.EyeImageKey = binding.Portal;
	const auto read = renderer.QueueResourceImage(view.Pipeline, core::Name("export"));
	REQUIRE(read != 0);
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
	const auto image = AwaitImage(renderer, read);
	const size_t centre = 18 * image.Width + 32;
	core::ByteReader colour(std::span(image.Pixels).subspan(centre * 8, 8));
	const auto rg = glm::unpackHalf2x16(colour.ReadUInt32());
	CHECK(rg.x < .001f);
	CHECK(rg.y < .001f);
	const auto ba = glm::unpackHalf2x16(colour.ReadUInt32());
	CHECK(ba.x > .5f);
	core::ByteReader depth(std::span(image.Depth).subspan(centre * 4, 4));
	CHECK(std::abs(depth.ReadFloat() - 6.5f) < .001f);
	REQUIRE(renderer.DropPortalImage(imported));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
}

TEST_CASE(
	"incoming portal images evict optional resident cache", "[render][gpu][resourceimage][resident-budget][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	render::SceneTarget target{512, 512};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("resident-budget");
	view.Pipeline = core::Name("image-export-pipeline");
	render::OverlayImage overlay;
	constexpr size_t mib = 1024 * 1024;
	auto bindingFor = [&](std::string_view name) {
		render::PortalImageBinding binding;
		binding.WorldName = view.WorldName;
		binding.Portal = core::Name(name);
		binding.Expected.PortalKey = name;
		binding.Expected.RequestId = 1;
		return binding;
	};
	auto adopt = [&](std::string_view name) {
		const auto token = renderer.QueueResourceImage(
			view.Pipeline, core::Name("image-export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(token != 0);
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		const auto handle = renderer.AdoptResourceImage(token, bindingFor(name));
		REQUIRE(handle != 0);
		return handle;
	};
	auto queueCopied = [&](std::string_view name) {
		const auto binding = bindingFor(name);
		render::PortalImageReply reply;
		reply.Key = binding.Expected;
		reply.Scope = binding.ExpectedScope;
		reply.Status = render::PortalImageStatus::Ok;
		reply.Width = reply.Height = 512;
		reply.RowStride = 512 * 8;
		reply.Pixels.resize(2 * mib);
		reply.Depth.resize(mib);
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
		const auto handle = renderer.QueuePortalImage(binding, std::move(reply));
		REQUIRE(handle != 0);
		return handle;
	};
	uint64_t resident = adopt("resident");
	resident = adopt("resident");
	REQUIRE(renderer.PortalImageUsage().CachedTextureBytes == 3 * mib);
	std::array<uint64_t, 9> copied{};
	for (size_t index = 0; index < copied.size(); ++index)
		copied[index] = queueCopied("copied-" + std::to_string(index));
	REQUIRE(renderer.PortalImageUsage().PendingCpuBytes == 27 * mib);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(renderer.PortalImageUsage().Uploads == 18);
	CHECK(renderer.PortalImageUsage().TextureBytes == 30 * mib);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);

	// A paired cache entry cannot satisfy a colour-only export. Adoption must
	// evict that optional pair when the incoming live image needs its space.
	REQUIRE(renderer.DropPortalImage(copied.back()));
	InstallImageCapture(renderer, "lit", false);
	resident = adopt("resident");
	REQUIRE(renderer.PortalImageUsage().TextureBytes == 26 * mib);
	REQUIRE(renderer.PortalImageUsage().CachedTextureBytes == 3 * mib);
	copied.back() = queueCopied("copied-8");
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(renderer.PortalImageUsage().TextureBytes == 29 * mib);
	REQUIRE(renderer.PortalImageUsage().CachedTextureBytes == 3 * mib);
	const auto other = adopt("other-resident");
	CHECK(renderer.PortalImageUsage().Images == 11);
	CHECK(renderer.PortalImageUsage().TextureBytes == 31 * mib);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
	for (const auto handle : copied)
		REQUIRE(renderer.DropPortalImage(handle));
	REQUIRE(renderer.DropPortalImage(resident));
	REQUIRE(renderer.DropPortalImage(other));
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
}

TEST_CASE(
	"resident capture groups transfer all ownership or none",
	"[render][gpu][resourceimage][resident-group][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	const auto pipeline = core::Name("image-export-pipeline");
	render::SceneTarget target{512, 512};
	render::View view;
	view.Pipeline = pipeline;
	view.Target = &target;
	render::OverlayImage overlay;
	uint64_t nextToken = 1000;
	auto capture = [&](size_t count) {
		std::vector<uint64_t> tokens;
		std::vector<render::ResourceImageRequest> requests;
		for (size_t index = 0; index < count; ++index) {
			tokens.push_back(nextToken++);
			requests.push_back(
				{tokens.back(),
				 pipeline,
				 core::Name("image-export"),
				 0,
				 render::ResourceImageDelivery::Resident}
			);
		}
		REQUIRE(renderer.RequestResourceImages(requests));
		const auto fenceProbe =
			count < 5 ? renderer.QueueResourceImage(pipeline, core::Name("image-export")) : 0;
		if (count < 5) REQUIRE(fenceProbe != 0);
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		if (fenceProbe)
			REQUIRE(AwaitImage(renderer, fenceProbe).Status == render::ResourceImageStatus::Ok);
		else
			REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(renderer.Backend().Device)));
		return tokens;
	};
	auto binding = [](size_t index) {
		render::PortalImageBinding value;
		value.WorldName = core::Name("group-owner");
		value.Portal = core::Name("layer-" + std::to_string(index / 3));
		value.Layer = static_cast<uint8_t>(index % 3);
		value.ExpectedScope = render::PortalImageScope::OpaqueLighting;
		value.Expected = {1, std::string(value.Portal.Text()), 1, 1};
		return value;
	};
	std::array<uint64_t, 9> previous{};
	for (size_t batch = 0; batch < 3; ++batch) {
		const auto tokens = capture(3);
		const std::array bindings{binding(batch * 3), binding(batch * 3 + 1), binding(batch * 3 + 2)};
		REQUIRE(renderer.AdoptResourceImages(tokens, bindings, std::span(previous).subspan(batch * 3, 3)));
	}
	const auto tokens = capture(2);
	std::array bindings{binding(0), binding(1)};
	CHECK_FALSE(renderer.AdoptResourceImages({}, {}, {}));
	std::array<uint64_t, 2> handles{777, 888};
	const auto retained = renderer.PortalImageUsage();
	CHECK(retained.TextureBytes == 27 * 1024 * 1024);
	CHECK_FALSE(renderer.AdoptResourceImages(tokens, bindings, handles));
	CHECK(handles == std::array<uint64_t, 2>{777, 888});
	CHECK(renderer.PortalImageUsage().TextureBytes == retained.TextureBytes);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == retained.CachedTextureBytes);
	for (const auto token : tokens)
		CHECK(renderer.CanPublishResourceImage(token, 512, 512));
	REQUIRE(renderer.DropPortalImage(previous.back()));
	for (const int invalid : {0, 1, 2, 3}) {
		auto changed = bindings;
		auto changedTokens = tokens;
		if (invalid == 0) changed[1].Expected.PortalKey = "wrong";
		if (invalid == 1) changed[1] = changed[0];
		if (invalid == 2) changedTokens[1] = changedTokens[0];
		if (invalid == 3) changed[1].Sampling = glm::mat4(0);
		CHECK_FALSE(renderer.AdoptResourceImages(changedTokens, changed, handles));
		CHECK(handles == std::array<uint64_t, 2>{777, 888});
		for (const auto token : tokens)
			CHECK(renderer.CanPublishResourceImage(token, 512, 512));
	}
	REQUIRE(renderer.AdoptResourceImages(tokens, bindings, handles));
	CHECK_FALSE(renderer.DropPortalImage(previous[0]));
	CHECK_FALSE(renderer.DropPortalImage(previous[1]));
	CHECK(renderer.PortalImageUsage().TextureBytes == 24 * 1024 * 1024);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 6 * 1024 * 1024);
	for (const auto token : tokens)
		CHECK_FALSE(renderer.CanPublishResourceImage(token, 512, 512));
	for (const auto handle : handles)
		CHECK(renderer.DropPortalImage(handle));
	for (size_t index = 2; index < 8; ++index)
		CHECK(renderer.DropPortalImage(previous[index]));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);

	const auto firstFrame = capture(1);
	const auto secondFrame = capture(1);
	const std::array mixedFrames{firstFrame.front(), secondFrame.front()};
	CHECK_FALSE(renderer.AdoptResourceImages(mixedFrames, bindings, handles));
	CHECK(renderer.PortalImageUsage().Images == 0);
	for (const auto token : mixedFrames) {
		CHECK(renderer.CanPublishResourceImage(token, 512, 512));
		CHECK(renderer.CancelResourceImage(token));
	}

	// Slot pressure is independent of the byte budget. A group cannot reserve
	// the last free import and then fail after consuming its first capture.
	target.Width = target.Height = 16;
	const auto fullCapture = capture(5);
	const std::array fullOwners{binding(0), binding(1), binding(2), binding(3), binding(4)};
	std::array<uint64_t, 5> fullHandles{};
	REQUIRE(renderer.AdoptResourceImages(fullCapture, fullOwners, fullHandles));
	CHECK(renderer.PortalImageUsage().Images == 5);
	for (const auto handle : fullHandles)
		CHECK(renderer.DropPortalImage(handle));
	CHECK(renderer.PortalImageUsage().Images == 0);
	std::array<uint64_t, render::MAX_IMPORTED_PORTAL_IMAGES - 1> occupied{};
	for (size_t first = 0; first < occupied.size(); first += 5) {
		const size_t count = std::min(size_t{5}, occupied.size() - first);
		const auto small = capture(count);
		std::vector<render::PortalImageBinding> owners;
		for (size_t index = 0; index < count; ++index)
			owners.push_back(binding(first + index));
		REQUIRE(renderer.AdoptResourceImages(small, owners, std::span(occupied).subspan(first, count)));
	}
	const auto small = capture(2);
	bindings = {binding(occupied.size()), binding(occupied.size() + 1)};
	const auto beforeSlotRefusal = handles;
	CHECK_FALSE(renderer.AdoptResourceImages(small, bindings, handles));
	CHECK(handles == beforeSlotRefusal);
	CHECK(renderer.PortalImageUsage().Images == occupied.size());
	for (const auto token : small)
		CHECK(renderer.CanPublishResourceImage(token, 16, 16));
	CHECK_FALSE(renderer.AdoptResourceImages(small, std::span(bindings).first(1), handles));
	CHECK_FALSE(renderer.AdoptResourceImages(small, bindings, std::span(handles).first(1)));
	REQUIRE(renderer.DropPortalImage(occupied.back()));
	REQUIRE(renderer.AdoptResourceImages(small, bindings, handles));
	CHECK(handles[0] != handles[1]);
	CHECK(renderer.PortalImageUsage().Images == render::MAX_IMPORTED_PORTAL_IMAGES);
	for (size_t index = 0; index + 1 < occupied.size(); ++index)
		CHECK(renderer.DropPortalImage(occupied[index]));
	for (const auto handle : handles)
		CHECK(renderer.DropPortalImage(handle));
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
}

TEST_CASE(
	"resident portal owners cancel each capture token once across retirement causes",
	"[render][gpu][portal-resident][retirement][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	const core::Name pipeline("image-export-pipeline");
	const core::Name output("image-export");
	std::array<uint64_t, 4> tokens{};
	for (auto &token : tokens) {
		token = renderer.QueueResourceImage(pipeline, output, 0, render::ResourceImageDelivery::Resident);
		REQUIRE(token != 0);
	}
	const uint64_t probe = renderer.QueueResourceImage(pipeline, output);
	REQUIRE(probe != 0);
	render::SceneTarget target{65, 37};
	render::View view;
	view.Pipeline = pipeline;
	view.Target = &target;
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(output));
	REQUIRE(AwaitImage(renderer, probe).Status == render::ResourceImageStatus::Ok);
	for (const uint64_t token : tokens)
		REQUIRE(renderer.CanPublishResourceImage(token, 65, 37));

	const render::PortalResidentImages::Time now{};
	const world::PresentationAddress source{"resident-source", "replies", 1, 1};
	render::PortalResidentImages resident(renderer);
#if ENGINE_ASSERTS_ENABLED
	render::test_support::PortalRendererTerminalTrace terminals(renderer);
#endif
	const auto publish = [&](size_t index, render::PortalResidentImages::Time at) {
		const world::PresentationAddress producer{"producer-" + std::to_string(index), "requests", 1, 1};
		render::PortalImageRequest request;
		request.Key = {index + 1, "Door", 1, 1};
		request.Width = 65;
		request.Height = 37;
		request.PixelBudget = 65 * 37;
		render::PortalImageBinding binding;
		binding.WorldName = core::Name(source.World);
		binding.Portal = core::Name("Door");
		binding.Expected = request.Key;
		REQUIRE(resident.Reserve(source, producer, request, binding, at));
		const render::PortalResidentReceipt receipt{request.Key, request.Scope, 0, 0, 0, 65, 37};
		REQUIRE(resident.Publish(source, producer, receipt, tokens[index], at));
		return producer;
	};
	const auto cancelledProducer = publish(0, now);
	const auto invalidatedProducer = publish(1, now);
	const auto expiredProducer = publish(2, now);
	(void)cancelledProducer;
	(void)expiredProducer;
	resident.Cancel(source, 1);
	resident.Cancel(source, 1);
	resident.Invalidate(invalidatedProducer);
	resident.Invalidate(invalidatedProducer);
	REQUIRE(resident.Expire(now + std::chrono::seconds(2)));
	CHECK(resident.Expire(now + std::chrono::seconds(2)));
	const auto clearedProducer = publish(3, now + std::chrono::seconds(2));
	(void)clearedProducer;
	resident.Clear();
	resident.Clear();
#if ENGINE_ASSERTS_ENABLED
	for (const uint64_t token : tokens) {
		CHECK(
			terminals.Count(render::test_support::PortalRendererTerminalKind::CancelResourceImage, token) == 1
		);
		CHECK(
			terminals.AppliedCount(
				render::test_support::PortalRendererTerminalKind::CancelResourceImage, token
			) == 1
		);
	}
#endif
}

TEST_CASE(
	"ambient occlusion follows the world feature layer and half-resolution quality",
	"[render][gpu][resourceimage]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), pipeline, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("ambient-quality");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));

	render::SceneTarget target{48, 32};
	render::View view;
	view.Pipeline = pipelineName;
	view.Target = &target;
	view.CameraFrame.Position = {0, 1, 4};
	view.OverrideLighting = true;
	// A wall standing on a floor: the crease between them is occluded.
	std::array<scene::DrawInstance, 2> instances{};
	instances[0].Source = 1;
	instances[0].Frame.Position = {0, 0, 0};
	instances[0].HalfExtent = {3, 0.05f, 3};
	instances[1].Source = 2;
	instances[1].Frame.Position = {0, 1, -1};
	instances[1].HalfExtent = {3, 1, 0.05f};
	for (auto &instance : instances)
		instance.CastShadow = false;
	view.Instances = instances;
	render::OverlayImage overlay;

	uint64_t token = 100;
	const auto capture = [&] {
		view.Damage.Scene = true;
		const render::ResourceImageRequest request{
			++token, pipelineName, core::Name("data-capture-ambient-occlusion"), 0
		};
		REQUIRE(renderer.RequestResourceImage(request));
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(request.Node));
		return AwaitImage(renderer, request.Token);
	};

	const auto full = capture();
	REQUIRE(full.AmbientOcclusion);
	CHECK(full.AmbientOcclusion->SourceState == render::AmbientOcclusionSourceState::Estimated);
	CHECK(full.AmbientOcclusion->Denoiser == render::AmbientOcclusionDenoiser::None);
	const size_t occluded =
		static_cast<size_t>(std::count_if(full.Pixels.begin(), full.Pixels.end(), [](std::byte pixel) {
			return pixel < std::byte{250};
		}));
	REQUIRE(occluded > 0);

	view.Lighting.HalfResolutionAmbientOcclusion = true;
	const auto half = capture();
	REQUIRE(half.AmbientOcclusion);
	CHECK(half.AmbientOcclusion->SourceState == render::AmbientOcclusionSourceState::Estimated);
	CHECK(half.AmbientOcclusion->Denoiser == render::AmbientOcclusionDenoiser::DepthAwareUpsample);
	REQUIRE(half.Pixels.size() == full.Pixels.size());
	// Upsampled occlusion stays close to the full trace on average.
	double difference = 0.0;
	for (size_t index = 0; index < full.Pixels.size(); ++index)
		difference += std::abs(int(full.Pixels[index]) - int(half.Pixels[index]));
	CHECK(difference / double(full.Pixels.size()) < 24.0);

	// The world layer alone switches the estimator off; the camera inherits.
	view.Lighting.HalfResolutionAmbientOcclusion = false;
	view.Lighting.RenderFeatures.Disable |= scene::FeatureBit(scene::RenderFeature::AmbientOcclusion);
	const auto disabled = capture();
	REQUIRE(disabled.AmbientOcclusion);
	CHECK(disabled.AmbientOcclusion->SourceState == render::AmbientOcclusionSourceState::ClearedDisabled);
}
