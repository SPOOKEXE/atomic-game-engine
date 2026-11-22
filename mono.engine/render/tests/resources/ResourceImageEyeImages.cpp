// Resident eye images: first use, camera following, directional samples,
// resize and body composition against retained walls.
#include "ResourceImageFixture.hpp"

TEST_SUITE_ID("engine.render.resourceimageeyeimages")

TEST_CASE(
	"a resident eye image draws on the first use of a new viewport",
	"[render][gpu][resourceimage][eye-first-use][.]"
) {
	using namespace engine;
	using namespace graph;
	const size_t nextSlot = GENERATE(size_t(3), size_t(4));
	const bool sceneOnly = GENERATE(false, true);
	const bool present = GENERATE(false, true);
	const bool captureHdr = GENERATE(false, true);
	CAPTURE(nextSlot, sceneOnly, present, captureHdr);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	auto document = DefaultEyeDocument();
	if (captureHdr) {
		for (const size_t slot : {size_t(3), size_t(4)}) {
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name("eye-export-" + std::to_string(slot)),
				 .NodeKind = core::Name("capture"),
				 .Scope = NodeScope::Frame}
			);
			document.Record(
				{.Kind = EditKind::Set, .Key = core::Name("view"), .Value = std::to_string(slot)}
			);
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name("eye-hdr"), .Key = core::Name("source")}
			);
		}
	}
	const auto displayPath =
		core::Paths::Base() / ("eye-first-use-" + std::to_string(nextSlot) + "-" + std::to_string(sceneOnly) +
							   "-" + std::to_string(present) + "-" + std::to_string(captureHdr) + ".bmp");
	if (!present) {
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name("display-export"),
			 .NodeKind = core::Name("capture"),
			 .Scope = NodeScope::Frame}
		);
		document.Record({.Kind = EditKind::Set, .Key = core::Name("path"), .Value = displayPath.string()});
		document.Record({.Kind = EditKind::Set, .Key = core::Name("capture.mode"), .Value = "every-frame"});
		document.Record(
			{.Kind = EditKind::Reads, .Target = core::Name("scene-image"), .Key = core::Name("source")}
		);
	}
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	const core::Name eyePipeline("first-eye-export");
	REQUIRE(renderer.SetPipeline(eyePipeline, pipeline));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("first-eye-emission");
	REQUIRE(renderer.AddTexture(emission, white));
	scene::DrawInstance wall;
	wall.Source = 1;
	wall.Frame.Position = {0, 0, -4};
	wall.HalfExtent = {20, 20, .05f};
	wall.CastShadow = false;
	wall.EmissiveMap = emission;
	wall.EmissiveTint = {0, 0, 1};
	wall.EmissiveStrength = 1.0f;
	render::SceneTarget target{65, 37};
	render::View source;
	source.World = 22;
	source.WorldName = core::Name("first-eye-producer");
	source.Target = &target;
	source.Pipeline = core::Name("image-export-pipeline");
	source.Instances = std::span(&wall, 1);
	source.OverrideLighting = true;
	source.Lighting.Ambient = {};
	source.Lighting.OutdoorAmbient = {};
	source.Lighting.Direct = {};
	render::OverlayImage overlay;
	const auto capture = [&](render::ResourceImageDelivery delivery) {
		const auto token =
			renderer.QueueResourceImage(source.Pipeline, core::Name("image-export"), 0, delivery);
		REQUIRE(token != 0);
		REQUIRE(
			renderer.Render(std::span(&source, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		return token;
	};
	const auto reference = AwaitImage(renderer, capture(render::ResourceImageDelivery::CopiedPixels));
	core::ByteReader centre(std::span(reference.Pixels).subspan((18 * 65 + 32) * 8, 8));
	(void)centre.ReadUInt32();
	REQUIRE(glm::unpackHalf2x16(centre.ReadUInt32()).x > .9f);
	uint64_t fallback = 0;
	for (const size_t slot : {size_t(3), nextSlot}) {
		render::PortalImageBinding binding;
		binding.World = 11;
		binding.WorldName = core::Name("first-eye-viewer");
		binding.ViewSlot = slot;
		binding.Portal = core::Name("viewport-eye");
		binding.Expected = {1, "viewport-eye", 1, 1};
		binding.ExpectedProjection = render::PortalImageProjection::Eye;
		binding.ExpectedScope = render::PortalImageScope::CompleteWorld;
		binding.Sampling =
			scene::ResolveCamera(source.CameraFrame, source.Camera, 65.f / 37.f).ViewProjection;
		const auto handle =
			renderer.AdoptResourceImage(capture(render::ResourceImageDelivery::Resident), binding);
		REQUIRE(handle != 0);
		render::View eye;
		eye.World = binding.World;
		eye.WorldName = binding.WorldName;
		eye.Slot = slot;
		eye.Target = &target;
		eye.Pipeline = eyePipeline;
		eye.EyeImage = handle;
		eye.EyeImageKey = binding.Portal;
		if (sceneOnly) eye.Damage = {.Scene = true};
		for (int frame = 0; frame < 2; ++frame) {
			CAPTURE(slot, frame);
			const core::Name exportNode("eye-export-" + std::to_string(slot));
			const auto token = captureHdr ? renderer.QueueResourceImage(eyePipeline, exportNode, slot) : 0;
			if (captureHdr) REQUIRE(token != 0);
			if (present) {
				REQUIRE(renderer.WaitForFrame());
				renderer.RequestSceneCapture(displayPath, slot);
			}
			const auto rendered = renderer.Render(std::span(&eye, 1), overlay, nullptr, present);
			REQUIRE(rendered.Ran(core::Name("eye-image")));
			if (captureHdr) {
				REQUIRE(rendered.Ran(exportNode));
				const auto actual = AwaitImage(renderer, token);
				CHECK(actual.Pixels == reference.Pixels);
			}
			std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> displayed(
				SDL_LoadBMP(displayPath.string().c_str()), SDL_DestroySurface
			);
			REQUIRE(displayed);
			Uint8 red = 0, green = 0, blue = 0, alpha = 0;
			REQUIRE(SDL_ReadSurfacePixel(displayed.get(), 32, 18, &red, &green, &blue, &alpha));
			CAPTURE(red, green, blue);
			CHECK(blue > 90);
			if (blue <= 90)
				std::filesystem::copy_file(
					displayPath,
					displayPath.string() + ".slot-" + std::to_string(slot) + ".frame-" +
						std::to_string(frame) + ".bmp",
					std::filesystem::copy_options::overwrite_existing
				);
			std::filesystem::remove(displayPath);
		}
		if (slot == 3 && nextSlot != 3)
			fallback = handle;
		else
			CHECK(renderer.DropPortalImage(handle));
	}
	if (fallback != 0) CHECK(renderer.DropPortalImage(fallback));
}

TEST_CASE(
	"a retained world packet follows the current viewing camera",
	"[render][gpu][resourceimage][eye-current-camera][.]"
) {
	using namespace engine;
	using namespace graph;
	const int motion = GENERATE(0, 1, 2, 3);
	CAPTURE(motion);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	auto document = DefaultEyeDocument();
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("image-export"),
		 .NodeKind = core::Name("capture"),
		 .Scope = NodeScope::Frame}
	);
	document.Record({.Kind = EditKind::Reads, .Target = core::Name("eye-hdr"), .Key = core::Name("source")});
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	const core::Name eyePipeline("current-eye-export");
	REQUIRE(renderer.SetPipeline(eyePipeline, pipeline));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("current-eye-emission");
	std::array<scene::DrawInstance, 3> room;
	for (size_t index = 0; index < room.size(); ++index) {
		room[index].Source = index + 1;
		room[index].CastShadow = false;
		room[index].EmissiveMap = emission;
		room[index].EmissiveStrength = 1.0f;
	}
	room[0].Frame.Position = {0, 0, -8};
	room[0].HalfExtent = {8, 8, .05f};
	room[0].EmissiveTint = {0, 0, 1};
	room[1].Frame.Position = {0, 0, -4};
	room[1].HalfExtent = {.6f, 1.5f, .05f};
	room[1].EmissiveTint = {1, 0, 0};
	room[2].Frame.Position = {0, 0, -6};
	room[2].HalfExtent = {.2f, 1, .05f};
	room[2].EmissiveTint = {0, 1, 0};
	render::SceneTarget target{65, 37};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("current-eye-room");
	view.ContentOwner = view.WorldName;
	REQUIRE(renderer.AddTexture(emission, white, view.ContentOwner));
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	const auto capture = [&](core::Name selected, std::span<const scene::DrawInstance> rows) {
		view.Pipeline = selected;
		view.Instances = rows;
		const auto token = renderer.QueueResourceImage(selected, core::Name("image-export"));
		REQUIRE(token != 0);
		render::OverlayImage overlay;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		return AwaitImage(renderer, token);
	};
	const core::Name directPipeline("image-export-pipeline");
	ecs::Store packetStore("current-eye-room");
	render::WorldViewFrame retained;
	retained.Name = view.WorldName;
	retained.Identity = packetStore.Identity();
	retained.Instances.assign(room.begin(), room.end());
	retained.Lighting = view.Lighting;
	render::WorldCameraFrame cameraLayers;
	const std::array contentOwners{
		render::WorldContentOwner{core::Name("foreign-room"), core::Name("foreign-room-content")}
	};
	render::WorldViewBinding packetBinding{
		.World = view.World,
		.Name = view.WorldName,
		.Identity = packetStore.Identity(),
		.ContentOwner = view.ContentOwner,
		.ForeignContentOwners = contentOwners,
		.Pipeline = directPipeline
	};
	const auto accepted = capture(directPipeline, room);
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("viewport-eye");
	binding.Expected = {1, "viewport-eye", 1, 1};
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	binding.ExpectedScope = render::PortalImageScope::CompleteWorld;
	binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 65.f / 37.f).ViewProjection;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Scope = binding.ExpectedScope;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = accepted.Width;
	reply.Height = accepted.Height;
	reply.RowStride = accepted.RowStride;
	reply.Pixels = accepted.Pixels;
	reply.Depth = accepted.Depth;
	reply.DepthHash = assets::Hasher::Of(reply.Depth);
	REQUIRE(reply.Depth.size() == size_t(accepted.Width) * accepted.Height * 4);
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
	view.EyeImageKey = binding.Portal;
	REQUIRE(view.EyeImage != 0);
	const auto retainedImage = view.EyeImage;
	if (motion == 1 || motion == 3) view.CameraFrame.Position.X = 3.f;
	if (motion == 2 || motion == 3) view.CameraFrame = view.CameraFrame * core::CFrame::Angles(0, .2f, 0);
	const auto expected = capture(directPipeline, room);
	const auto staleImage = capture(eyePipeline, {});
	// Pixels alone retain their capture camera; the hidden post requires the world packet.
	CHECK(staleImage.Pixels == accepted.Pixels);
	const auto currentCamera = view.CameraFrame;
	const auto currentProjection = view.Projection;
	const auto currentTarget = view.Target;
	const auto currentSlot = view.Slot;
	auto wrongOwner = packetBinding;
	wrongOwner.Name = core::Name("unrelated-eye-room");
	CHECK_FALSE(render::BindWorldView(retained, cameraLayers, wrongOwner, view));
	CHECK(view.EyeImage == retainedImage);
	CHECK(view.Pipeline == eyePipeline);
	ecs::Store replacementStore("current-eye-room");
	auto wrongStore = packetBinding;
	wrongStore.Identity = replacementStore.Identity();
	CHECK_FALSE(render::BindWorldView(retained, cameraLayers, wrongStore, view));
	CHECK(view.EyeImage == retainedImage);
	CHECK(view.Pipeline == eyePipeline);
	REQUIRE(render::BindWorldView(retained, cameraLayers, packetBinding, view));
	CHECK(view.CameraFrame.Position == currentCamera.Position);
	CHECK(view.CameraFrame.Rotation() == currentCamera.Rotation());
	CHECK(view.Projection == currentProjection);
	CHECK(view.Target == currentTarget);
	CHECK(view.Slot == currentSlot);
	CHECK(view.EyeImage == 0);
	CHECK(view.EyeImageKey == core::Name{});
	CHECK(view.ContentOwner == packetBinding.ContentOwner);
	CHECK(view.ForeignContentOwners.data() == contentOwners.data());
	CHECK(view.Instances.data() == retained.Instances.data());
	const auto actual = capture(view.Pipeline, view.Instances);
	const auto unpack = [](const render::ResourceImage &image) {
		std::vector<float> samples;
		core::ByteReader reader(image.Pixels);
		while (!reader.AtEnd()) {
			const auto pair = glm::unpackHalf2x16(reader.ReadUInt32());
			samples.push_back(pair.x);
			samples.push_back(pair.y);
		}
		return samples;
	};
	const auto expectedSamples = unpack(expected), actualSamples = unpack(actual);
	if (motion == 1 || motion == 3) {
		const auto greenPixels = [](const std::vector<float> &samples) {
			size_t count = 0;
			for (size_t pixel = 0; pixel < samples.size(); pixel += 4)
				if (samples[pixel + 1] > .9f && samples[pixel] < .1f && samples[pixel + 2] < .1f) ++count;
			return count;
		};
		CHECK(greenPixels(unpack(accepted)) == 0);
		REQUIRE(greenPixels(expectedSamples) > 4);
	}
	const auto asImage = [](const render::ResourceImage &image, const std::vector<float> &samples) {
		return render::test::ImageView{
			image.Width,
			image.Height,
			render::test::ImageFormat::Rgba32Float,
			std::as_bytes(std::span(samples)),
			size_t(image.Width) * 16
		};
	};
	if (motion != 0) CHECK(expected.Pixels != accepted.Pixels);
	render::test::CheckImage(
		renderer,
		"eye-current-camera",
		"motion-" + std::to_string(motion),
		"retained world packet at current camera; red near occluder, green hidden post, blue rear wall",
		asImage(expected, expectedSamples),
		asImage(actual, actualSamples),
		{.Absolute = .002, .Region = {}}
	);
	CHECK(renderer.DropPortalImage(retainedImage));
}

TEST_CASE(
	"eye images preserve directional samples through upload and resident adoption",
	"[render][gpu][resourceimage][eye-directional-response][.]"
) {
	using namespace graph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const std::array names{
		"colour", "depth", "normal", "ambient-response", "lighting-baseline", "directional-response"
	};
	const std::array formats{
		ResourceFormat::RGBA16F,
		ResourceFormat::R32F,
		ResourceFormat::RGB10A2,
		ResourceFormat::RGBA32F,
		ResourceFormat::RGBA32F,
		ResourceFormat::RGBA32F
	};
	PipelineDocument document;
	for (size_t plane = 0; plane < names.size(); ++plane)
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name(names[plane]),
			 .Resource = ResourceKind::Colour,
			 .Format = formats[plane]}
		);
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("eye"),
		 .NodeKind = core::Name("eye-image"),
		 .Scope = NodeScope::View}
	);
	document.Record({.Kind = EditKind::Set, .Key = core::Name("scope"), .Value = "opaque-lighting"});
	for (const char *name : names)
		document.Record({.Kind = EditKind::Writes, .Target = core::Name(name), .Key = core::Name(name)});
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("export"),
		 .NodeKind = core::Name("capture"),
		 .Scope = NodeScope::Frame}
	);
	for (size_t plane = 0; plane < names.size(); ++plane)
		document.Record(
			{.Kind = EditKind::Reads,
			 .Target = core::Name(names[plane]),
			 .Key = core::Name(plane == 0 ? "source" : names[plane])}
		);
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	const core::Name pipelineName("eye-directional-samples");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));
	constexpr uint32_t width = 17, height = 9;
	render::SceneTarget target{width, height};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("directional-samples-world");
	view.Pipeline = pipelineName;
	view.EyeImageKey = core::Name("directional-samples-image");
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = view.EyeImageKey;
	binding.Expected = {1, "directional-samples-image", 1, 1};
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Scope = binding.ExpectedScope;
	reply.CaptureLighting.emplace();
	reply.Width = width;
	reply.Height = height;
	reply.RowStride = width * 8;
	core::ByteWriter colour, depth, normal, ambient, baseline, directional;
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x) {
			colour.WriteUInt32(glm::packHalf2x16({.25f, .5f}));
			colour.WriteUInt32(glm::packHalf2x16({.75f, 1.f}));
			depth.WriteFloat(2.f + float(x + y) / 16.f);
			normal.WriteUInt32(0xfff80200u);
			for (float value : {.125f, .25f, .375f, .5f})
				ambient.WriteFloat(value);
			for (float value : {.25f, .5f, .75f, 1.f})
				baseline.WriteFloat(value);
			for (float value :
				 {float(x + 1) / 8.f,
				  float(y + 1) / 16.f,
				  float(x + y + 1) / 32.f,
				  float((x + 3 * y) % 17) / 16.f})
				directional.WriteFloat(value);
		}
	const std::array sources{&colour, &depth, &normal, &ambient, &baseline, &directional};
	const std::array planes{
		&reply.Pixels,
		&reply.Depth,
		&reply.Normal,
		&reply.AmbientResponse,
		&reply.LightingBaseline,
		&reply.DirectionalResponse
	};
	const std::array hashes{
		&reply.PixelHash,
		&reply.DepthHash,
		&reply.NormalHash,
		&reply.AmbientResponseHash,
		&reply.LightingBaselineHash,
		&reply.DirectionalResponseHash
	};
	for (size_t plane = 0; plane < planes.size(); ++plane) {
		planes[plane]->assign(sources[plane]->Bytes().begin(), sources[plane]->Bytes().end());
		*hashes[plane] = assets::Hasher::Of(*planes[plane]);
	}
	const auto expected = reply;
	view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
	REQUIRE(view.EyeImage != 0);
	const auto resident = renderer.QueueResourceImage(
		pipelineName, core::Name("export"), 0, render::ResourceImageDelivery::Resident
	);
	REQUIRE(resident != 0);
	for (int delivery = 0; delivery < 2; ++delivery) {
		CAPTURE(delivery);
		const auto copied = renderer.QueueResourceImage(pipelineName, core::Name("export"));
		REQUIRE(copied != 0);
		render::OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
		const auto actual = AwaitImage(renderer, copied);
		REQUIRE(actual.Status == render::ResourceImageStatus::Ok);
		CHECK(actual.Pixels == expected.Pixels);
		CHECK(actual.Depth == expected.Depth);
		CHECK(actual.Normal == expected.Normal);
		CHECK(actual.AmbientResponse == expected.AmbientResponse);
		CHECK(actual.LightingBaseline == expected.LightingBaseline);
		CHECK(actual.DirectionalResponseResource == core::Name("directional-response"));
		CHECK(actual.DirectionalResponse == expected.DirectionalResponse);
		CHECK(assets::Hasher::Of(actual.DirectionalResponse) == expected.DirectionalResponseHash);
		CHECK(renderer.PortalImageUsage().Uploads == 6);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		if (delivery == 0) {
			binding.Expected.RequestId++;
			view.EyeImage = renderer.AdoptResourceImage(resident, binding);
			REQUIRE(view.EyeImage != 0);
		}
	}
	CHECK(renderer.DropPortalImage(view.EyeImage));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
}

TEST_CASE(
	"resizing an eye image preserves each color and depth sample pair",
	"[render][gpu][resourceimage][eye-pair-resize][.]"
) {
	using namespace graph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const bool resident = GENERATE(false, true);
	const bool reduce = GENERATE(false, true);
	CAPTURE(resident, reduce);
	PipelineDocument document;
	for (const auto &[name, format] :
		 {std::pair{"eye-colour", ResourceFormat::RGBA16F}, {"eye-depth", ResourceFormat::R32F}})
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name(name),
			 .Resource = ResourceKind::Colour,
			 .Format = format}
		);
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("eye"),
		 .NodeKind = core::Name("eye-image"),
		 .Scope = NodeScope::View}
	);
	document.Record(
		{.Kind = EditKind::Writes, .Target = core::Name("eye-colour"), .Key = core::Name("colour")}
	);
	document.Record(
		{.Kind = EditKind::Writes, .Target = core::Name("eye-depth"), .Key = core::Name("depth")}
	);
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("export"),
		 .NodeKind = core::Name("capture"),
		 .Scope = NodeScope::Frame}
	);
	document.Record(
		{.Kind = EditKind::Reads, .Target = core::Name("eye-colour"), .Key = core::Name("source")}
	);
	document.Record({.Kind = EditKind::Reads, .Target = core::Name("eye-depth"), .Key = core::Name("depth")});
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	REQUIRE(renderer.SetPipeline(core::Name("eye-resize"), pipeline));
	const uint32_t width = reduce ? 3 : 13, height = reduce ? 3 : 7;
	render::SceneTarget target{width, height};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("eye-room");
	view.Pipeline = core::Name("eye-resize");
	view.EyeImageKey = core::Name("eye-room-image");
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = view.EyeImageKey;
	binding.Expected = {1, "eye-room-image", 1, 1};
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = 8;
	reply.Height = 8;
	reply.RowStride = 64;
	const std::array colors{
		glm::vec4{1, 0, 0, 1}, glm::vec4{0, 0, 1, 1}, glm::vec4{0, 1, 0, 1}, glm::vec4{1, 1, 0, 1}
	};
	const std::array distances{2.f, 6.f, 10.f, 0.f};
	core::ByteWriter colorBytes, depthBytes;
	for (size_t pixel = 0; pixel < 64; ++pixel) {
		const size_t i = (pixel % 8 >= 4 ? 1 : 0) + (pixel / 8 >= 4 ? 2 : 0);
		colorBytes.WriteUInt32(glm::packHalf2x16({colors[i].r, colors[i].g}));
		colorBytes.WriteUInt32(glm::packHalf2x16({colors[i].b, colors[i].a}));
		depthBytes.WriteFloat(distances[i]);
	}
	reply.Pixels.assign(colorBytes.Bytes().begin(), colorBytes.Bytes().end());
	reply.Depth.assign(depthBytes.Bytes().begin(), depthBytes.Bytes().end());
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	reply.DepthHash = assets::Hasher::Of(reply.Depth);
	view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
	REQUIRE(view.EyeImage != 0);
	render::OverlayImage overlay;
	if (resident) {
		render::SceneTarget residentTarget{8, 8};
		view.Target = &residentTarget;
		const auto residentToken = renderer.QueueResourceImage(
			view.Pipeline, core::Name("export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(residentToken != 0);
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
		const auto image = renderer.AdoptResourceImage(residentToken, binding);
		REQUIRE(image != 0);
		CHECK(image != view.EyeImage);
		CHECK(renderer.PortalImageUsage().Images == 1);
		view.EyeImage = image;
		view.Target = &target;
	}
	const auto token = renderer.QueueResourceImage(view.Pipeline, core::Name("export"));
	REQUIRE(token != 0);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
	const auto resized = AwaitImage(renderer, token);
	REQUIRE(resized.Width == width);
	REQUIRE(resized.Height == height);
	core::ByteReader color(resized.Pixels), depth(resized.Depth);
	std::array<size_t, 4> selected{};
	std::vector<glm::vec4> samples;
	while (!depth.AtEnd()) {
		const float distance = depth.ReadFloat();
		const auto found = std::find(distances.begin(), distances.end(), distance);
		REQUIRE(found != distances.end());
		const auto index = size_t(found - distances.begin());
		++selected[index];
		const auto rg = glm::unpackHalf2x16(color.ReadUInt32());
		const auto ba = glm::unpackHalf2x16(color.ReadUInt32());
		samples.emplace_back(rg, ba);
		CHECK(glm::vec4(rg, ba) == colors[index]);
	}
	if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
		const auto directory = std::filesystem::path(output);
		std::filesystem::create_directories(directory);
		const std::string stem =
			std::string(resident ? "resident-" : "copied-") + (reduce ? "reduced" : "enlarged");
		const render::test::ImageView preview{
			width, height, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
		};
		render::test::WriteImagePreview(directory / (stem + ".ppm"), preview);
	}
	CHECK(color.AtEnd());
	for (const auto count : selected)
		CHECK(count > 0);
	REQUIRE(renderer.DropPortalImage(view.EyeImage));
}

TEST_CASE(
	"a current opaque body composes against a retained destination wall",
	"[slow][render][gpu][resourceimage][body-depth-compose][.]"
) {
	using namespace graph;
	const bool opaqueScope = GENERATE(false, true);
	const bool shaded = GENERATE(false, true);
	const bool humanoid = GENERATE(false, true);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const auto install = [&](bool compose, bool opaque, bool seam = false) {
		PipelineDocument document;
		if (compose) {
			const auto shared = DefaultPortalBodyDocument(seam);
			for (auto edit : shared.Edits()) {
				// Complete-world fixtures contain only opaque geometry.
				if (!opaque && edit.Kind == EditKind::Set && edit.Key == core::Name("scope"))
					edit.Value = "complete-world";
				document.Record(std::move(edit));
			}
		} else {
			const auto resource = [&](const char *name, ResourceFormat format) {
				document.Record(
					{.Kind = EditKind::AddResource,
					 .Name = core::Name(name),
					 .Resource = ResourceKind::Colour,
					 .Format = format}
				);
			};
			const auto node = [&](const char *name, const char *kind) {
				document.Record(
					{.Kind = EditKind::AddNode,
					 .Name = core::Name(name),
					 .NodeKind = core::Name(kind),
					 .Scope = NodeScope::View}
				);
			};
			const auto edge = [&](EditKind kind, const char *name, const char *port) {
				document.Record({.Kind = kind, .Target = core::Name(name), .Key = core::Name(port)});
			};
			const auto base = DefaultPbrDocument();
			for (const auto &edit : base.Edits()) {
				if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("present")) {
					resource("opaque-depth", ResourceFormat::R32F);
					node("opaque-depth-export", "depth-linearise");
					edge(EditKind::Reads, "depth", "depth");
					edge(EditKind::Writes, "opaque-depth", "linear");
					document.Record(
						{.Kind = EditKind::Set, .Key = core::Name("background"), .Value = "zero"}
					);
				}
				document.Record(edit);
			}
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name("export"),
				 .NodeKind = core::Name("capture"),
				 .Scope = NodeScope::Frame}
			);
			edge(EditKind::Reads, "lit", "source");
			edge(EditKind::Reads, "opaque-depth", "depth");
		}

		RenderGraph pipeline;
		core::Name offender;
		REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
		const core::Name name(compose ? (seam ? "body-seam-compose" : "body-compose") : "body-reference");
		REQUIRE(renderer.SetPipeline(name, pipeline));
		return name;
	};
	const auto referencePipeline = install(false, opaqueScope);
	const auto compositionPipeline = install(true, opaqueScope);
	REQUIRE(install(true, opaqueScope, true) == core::Name("body-seam-compose"));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("body-emission");
	REQUIRE(renderer.AddTexture(emission, white));
	render::SceneTarget target{65, 37};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("body-source");
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	const bool rotated = GENERATE(false, true);
	if (rotated)
		view.CameraFrame = core::CFrame(core::Vector3{7, 2, 3}) * core::CFrame::Angles(-.2f, .37f, .1f);
	const auto capture = [&](core::Name pipeline, std::span<const scene::DrawInstance> rows) {
		view.Pipeline = pipeline;
		view.Instances = rows;
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("export"));
		REQUIRE(token != 0);
		render::OverlayImage overlay;
		const auto frame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("export")));
		return AwaitImage(renderer, token);
	};
	scene::DrawInstance wall;
	wall.Source = 1;
	wall.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -4});
	wall.HalfExtent = {1, 1, .05f};
	wall.CastShadow = false;
	wall.EmissiveMap = emission;
	wall.EmissiveTint = {1, 0, 0};
	if (shaded) {
		view.Lighting.Direction = view.CameraFrame.VectorToWorldSpace({-.3f, -.4f, -1}).Unit();
		view.Lighting.Direct = {4, 4, 4};
		wall.EmissiveMap = {};
		wall.Tint = {1, 0, 0};
	} else
		wall.EmissiveStrength = 1.0f;
	const bool resident = GENERATE(false, true);
	const auto residentToken =
		resident ? renderer.QueueResourceImage(
					   referencePipeline, core::Name("export"), 0, render::ResourceImageDelivery::Resident
				   )
				 : 0;
	if (resident) REQUIRE(residentToken != 0);
	const auto room = capture(referencePipeline, std::span(&wall, 1));
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("room");
	binding.Expected = {1, "room", 1, 1};
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	binding.ExpectedScope =
		opaqueScope ? render::PortalImageScope::OpaqueLighting : render::PortalImageScope::CompleteWorld;
	binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 65.f / 37.f).ViewProjection;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Scope = binding.ExpectedScope;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = room.Width;
	reply.Height = room.Height;
	reply.RowStride = room.RowStride;
	reply.Pixels = room.Pixels;
	reply.Depth = room.Depth;
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	reply.DepthHash = assets::Hasher::Of(reply.Depth);
	view.EyeImage = resident ? renderer.AdoptResourceImage(residentToken, binding)
							 : renderer.QueuePortalImage(binding, std::move(reply));
	view.EyeImageKey = binding.Portal;
	REQUIRE(view.EyeImage != 0);
	view.Pipeline = core::Name("body-seam-compose");
	render::OverlayImage projectionOverlay;
	CHECK_FALSE(renderer.Render(std::span(&view, 1), projectionOverlay, nullptr, false)
					.Ran(core::Name("body-compose")));
	view.Pipeline = install(true, !opaqueScope);
	render::OverlayImage scopeOverlay;
	CHECK_FALSE(
		renderer.Render(std::span(&view, 1), scopeOverlay, nullptr, false).Ran(core::Name("body-compose"))
	);
	REQUIRE(install(true, opaqueScope) == compositionPipeline);
	scene::DrawInstance body = wall;
	body.Source = 2;
	body.HalfExtent = {.6f, 1.4f, .1f};
	body.EmissiveTint = {0, 0, 1};
	if (shaded) body.Tint = {0, 0, 1};
	const auto unpack = [](const render::ResourceImage &image) {
		std::vector<float> samples;
		samples.reserve(image.Pixels.size() / 2);
		core::ByteReader reader(image.Pixels);
		while (!reader.AtEnd()) {
			const auto pair = glm::unpackHalf2x16(reader.ReadUInt32());
			samples.push_back(pair.x);
			samples.push_back(pair.y);
		}
		return samples;
	};
	std::unique_ptr<ecs::Store> characterWorld;
	ecs::Entity characterRoot, characterHumanoid;
	std::vector<std::pair<ecs::Entity, core::CFrame>> limbOffsets;
	if (humanoid) {
		scene::RegisterSceneClasses();
		characterWorld = std::make_unique<ecs::Store>("current-body");
		auto &store = *characterWorld;
		scene::InstallServices(store);
		render::RegisterPresentationComponents();
		store.SetResource(render::DrawList{});
		const auto player = scene::AddPlayer(store, "viewer", false, 91);
		scene::CharacterDesc description;
		description.Frame.Position = {0, -2.5f, 0};
		const auto model = scene::MakeCharacter(store, description);
		REQUIRE(scene::SetPlayerCharacter(store, player, model));
		const auto *character = store.Get<scene::Character>(model);
		REQUIRE(character != nullptr);
		characterRoot = character->Root;
		characterHumanoid = character->Humanoid;
		store.Each<const scene::CharacterLimb>([&](ecs::Entity entity, const scene::CharacterLimb &limb) {
			limbOffsets.emplace_back(entity, limb.Offset);
		});
	}
	std::optional<assets::ContentHash> firstPoseHash;
	std::vector<scene::DrawInstance> bodyRows;
	std::vector<scene::DrawInstance> apertureRows;
	std::optional<uint64_t> warmTextures;
	for (size_t pose = 0; pose < 4; ++pose) {
		const float distance = pose == 1 ? 5.f : 3.f;
		CAPTURE(rotated, resident, distance, humanoid, pose, shaded);
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{.7f, 0, -distance});
		bodyRows.assign(1, body);
		if (humanoid) {
			auto &store = *characterWorld;
			const float swing = pose == 0 || pose == 3 ? -.7f : .7f;
			for (const auto &[entity, rest] : limbOffsets) {
				auto *limb = store.GetMutable<scene::CharacterLimb>(entity);
				limb->Offset = rest;
				if (store.InstanceNameOf(entity) == core::Name("Right Arm"))
					limb->Offset = rest * core::CFrame::Angles(swing, 0, 0);
			}
			scene::PoseCharacters(store);
			scene::CapturePreviousTransforms(store);
			scene::SyncRendered(store);
			render::CollectInstances(store);
			bodyRows = store.Resource<render::DrawList>()->Instances;
			REQUIRE(bodyRows.size() == 6);
			const core::Vector3 normal = view.CameraFrame.VectorToWorldSpace({1, 0, 0});
			const auto cut = view.CameraFrame.PointToWorldSpace({.7f, 0, -distance});
			scene::SeamTransform through;
			through.Frame = view.CameraFrame * core::CFrame(core::Vector3{.7f, 0, -distance});
			through.Scale = .5f;
			for (auto &row : bodyRows) {
				REQUIRE(row.Rig == characterRoot.Id);
				row.Frame = through.Place(row.Frame);
				row.HalfExtent = row.HalfExtent * through.Scale;
				row.CastShadow = false;
				row.EmissiveMap = emission;
				row.EmissiveTint = {0, 0, 1};
				if (shaded) {
					row.EmissiveMap = {};
					row.Tint = {0, 0, 1};
				} else
					row.EmissiveStrength = 1.0f;
				row.SeamNormal = normal;
				row.SeamOffset = normal.Dot(cut);
			}
		}
		auto together = bodyRows;
		together.push_back(wall);
		const auto reference = capture(referencePipeline, together);
		const auto composed = capture(compositionPipeline, bodyRows);
		if (humanoid && !opaqueScope && !shaded && pose == 2) {
			apertureRows = together;
		}
		const auto poseHash = assets::Hasher::Of(composed.Pixels);
		if (pose == 0) firstPoseHash = poseHash;
		if (pose == 2 && humanoid) CHECK(poseHash != *firstPoseHash);
		if (pose == 3) CHECK(poseHash == *firstPoseHash);
		const auto expectedColour = unpack(reference), actualColour = unpack(composed);
		if (shaded && pose == 0) {
			const auto uploads = renderer.PortalImageUsage().Uploads;
			const auto sunlight = view.Lighting.Direct;
			view.Lighting.Direct = {};
			const auto unlit = capture(compositionPipeline, bodyRows);
			view.Lighting.Direct = sunlight;
			const auto darkColour = unpack(unlit);
			size_t darkenedBody = 0, retainedWall = 0;
			for (size_t pixel = 0; pixel < actualColour.size(); pixel += 4) {
				if (actualColour[pixel + 2] > .5f && actualColour[pixel] < .1f) {
					CHECK(darkColour[pixel + 2] < .001f);
					++darkenedBody;
				}
				if (actualColour[pixel] > .5f && actualColour[pixel + 2] < .1f) {
					CHECK(darkColour[pixel] == actualColour[pixel]);
					++retainedWall;
				}
			}
			CHECK(darkenedBody > 0);
			CHECK(retainedWall > 0);
			CHECK(unlit.Depth == composed.Depth);
			CHECK(renderer.PortalImageUsage().Uploads == uploads);
			CHECK(capture(compositionPipeline, bodyRows).Pixels == composed.Pixels);
		}
		const render::test::ImageView expectedView{
			65, 37, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(expectedColour)), 0
		};
		const render::test::ImageView actualView{
			65, 37, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(actualColour)), 0
		};
		render::test::CheckImage(
			renderer, "body-depth-compose", "radiance", "opaque body and wall", expectedView, actualView
		);
		size_t red = 0, blue = 0;
		for (size_t pixel = 0; pixel < actualColour.size(); pixel += 4) {
			red += actualColour[pixel] > .5f && actualColour[pixel + 2] < .1f;
			blue += actualColour[pixel + 2] > .5f && actualColour[pixel] < .1f;
		}
		CHECK(red > 0);
		CHECK(blue > 0);
		if (humanoid && pose == 0) {
			auto uncut = together;
			for (auto &row : uncut) {
				row.SeamNormal = {};
				row.SeamOffset = 0;
			}
			const auto fullBody = unpack(capture(referencePipeline, uncut));
			size_t fullBlue = 0;
			for (size_t pixel = 0; pixel < fullBody.size(); pixel += 4)
				fullBlue += fullBody[pixel + 2] > .5f && fullBody[pixel] < .1f;
			CHECK(fullBlue > blue);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const std::string stem = std::string(humanoid ? "humanoid-" : "block-") +
									 (rotated ? "rotated-" : "identity-") +
									 (shaded ? "sunlit-" : "emissive-") + std::to_string(pose);
			render::test::WriteImagePreview(directory / (stem + "-direct.ppm"), expectedView);
			render::test::WriteImagePreview(directory / (stem + "-composed.ppm"), actualView);
		}
		core::ByteReader expected(reference.Depth), actual(composed.Depth);
		REQUIRE(composed.Depth.size() == reference.Depth.size());
		while (!expected.AtEnd())
			CHECK(std::abs(expected.ReadFloat() - actual.ReadFloat()) < .0001f);
		const auto textures = renderer.MemoryStatistics().TextureAllocations;
		if (warmTextures)
			CHECK(textures == *warmTextures);
		else
			warmTextures = textures;
	}
	if (humanoid) {
		auto &store = *characterWorld;
		const auto eye = store.CreateInstance(scene::CameraClass(), "FirstPerson");
		store.Set(eye, scene::CameraSubject{characterHumanoid});
		store.SetResource(scene::ActiveCamera{eye});
		scene::CameraController controller;
		controller.Mode = scene::CameraMode::LockFirstPerson;
		store.SetResource(controller);
		render::SelectFirstPersonBody(store, view);
		REQUIRE(view.EyeRig == characterRoot.Id);
		REQUIRE(view.EyePlayer == 91);
		const auto hidden = capture(compositionPipeline, bodyRows);
		CHECK(hidden.Pixels == room.Pixels);
		CHECK(hidden.Depth == room.Depth);
		view.EyeRig = 0;
		view.EyePlayer.reset();
	}
	CHECK(renderer.PortalImageUsage().Uploads == (resident ? 0 : 2));
	if (!apertureRows.empty()) {
		for (const float scale : {.25f, 1.f, 4.f}) {
			CheckBodyAperture(renderer, view, apertureRows, rotated, scale, resident);
			render::SelectFirstPersonBody(*characterWorld, view);
			REQUIRE(view.EyeRig == characterRoot.Id);
			CheckBodyAperture(renderer, view, apertureRows, rotated, scale, resident);
			view.EyeRig = 0;
			view.EyePlayer.reset();
		}
	}
	REQUIRE(renderer.DropPortalImage(view.EyeImage));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	for (const bool colourOnly : {false, true}) {
		view.EyeImage = 0;
		if (colourOnly) {
			render::PortalImageReply unpaired;
			unpaired.Key = binding.Expected;
			unpaired.Scope = binding.ExpectedScope;
			unpaired.Status = render::PortalImageStatus::Ok;
			unpaired.Width = room.Width;
			unpaired.Height = room.Height;
			unpaired.RowStride = room.RowStride;
			unpaired.Pixels = room.Pixels;
			unpaired.PixelHash = assets::Hasher::Of(unpaired.Pixels);
			view.EyeImage = renderer.QueuePortalImage(binding, std::move(unpaired));
			REQUIRE(view.EyeImage != 0);
		}
		view.Pipeline = compositionPipeline;
		view.Instances = std::span(&body, 1);
		render::OverlayImage overlay;
		CHECK_FALSE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("body-compose"))
		);
		if (colourOnly) REQUIRE(renderer.DropPortalImage(view.EyeImage));
	}
}
