// Transparent layers, peeled glass, oblique floor canvases and copied portal
// layers around the current body.
#include "ResourceImageFixture.hpp"

TEST_SUITE_ID("engine.render.resourceimagetransparency")

TEST_CASE(
	"ordered transparent layers preserve opaque body depth",
	"[render][gpu][resourceimage][transparent-depth-compose][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	graph::PipelineDocument document;
	const auto opaqueDocument = graph::DefaultPortalBodyDocument();
	for (const auto &edit : opaqueDocument.Edits()) {
		document.Record(edit);
		if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("body-compose"))
			document.Record(
				{.Kind = graph::EditKind::Set, .Key = core::Name("mode"), .Value = "transparent"}
			);
	}
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name composition("transparent-depth-compose");
	REQUIRE(renderer.SetPipeline(composition, graph));
	assets::TextureData texture;
	texture.Width = texture.Height = 1;
	texture.Format = assets::TextureFormat::RGBA8;
	texture.Pixels.assign(4, std::byte{255});
	const core::Name emission("layer-emission"), mask("layer-alpha");
	REQUIRE(renderer.AddTexture(emission, texture));
	const uint8_t opacity = GENERATE(uint8_t{0}, uint8_t{128}, uint8_t{255});
	texture.Pixels[3] = std::byte{opacity};
	REQUIRE(renderer.AddTexture(mask, texture));
	render::SceneTarget target{33, 33};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("layer-world");
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	scene::DrawInstance layer;
	layer.Source = 1;
	layer.HalfExtent = {.5f, .5f, .5f};
	layer.CastShadow = false;
	layer.Tint = {};
	layer.Texture = mask;
	layer.Alpha = scene::AlphaMode::Transparency;
	layer.AlphaCutoff = 0;
	layer.EmissiveMap = emission;
	view.Instances = std::span(&layer, 1);
	const auto capture = [&](core::Name pipeline, core::Name node) {
		view.Pipeline = pipeline;
		const auto token = renderer.QueueResourceImage(pipeline, node);
		REQUIRE(token != 0);
		render::OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(node));
		return AwaitImage(renderer, token);
	};
	const auto colourAt = [](const render::ResourceImage &image, size_t pixel) {
		core::ByteReader reader(std::span(image.Pixels).subspan(pixel * 8, 8));
		const auto rg = glm::unpackHalf2x16(reader.ReadUInt32());
		return glm::vec4(rg, glm::unpackHalf2x16(reader.ReadUInt32()));
	};
	const auto depthAt = [](const render::ResourceImage &image, size_t pixel) {
		core::ByteReader reader(std::span(image.Depth).subspan(pixel * 4, 4));
		return reader.ReadFloat();
	};
	const float opaqueDepth = GENERATE(0.f, 2.f, 2.5f, 5.f);
	const float backgroundOpacity = opaqueDepth == 0 ? GENERATE(0.f, 1.f) : 1.f;
	CAPTURE(opaqueDepth, opacity, backgroundOpacity);
	render::ResourceImage background;
	background.Width = background.Height = 33;
	background.RowStride = 33 * 8;
	core::ByteWriter pixels, depths;
	for (size_t pixel = 0; pixel < 33 * 33; ++pixel) {
		pixels.WriteUInt32(glm::packHalf2x16({0, 0}));
		pixels.WriteUInt32(glm::packHalf2x16({2 * backgroundOpacity, backgroundOpacity}));
		depths.WriteFloat(opaqueDepth);
	}
	background.Pixels.assign(pixels.Bytes().begin(), pixels.Bytes().end());
	background.Depth.assign(depths.Bytes().begin(), depths.Bytes().end());
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("layer-background");
	binding.Expected.PortalKey = "layer-background";
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	view.EyeImageKey = binding.Portal;
	// Two layers are applied back to front over a body, a far wall or empty depth.
	for (int index = 0; index < 2; ++index) {
		layer.Frame.Position = {0, 0, index == 0 ? -4.f : -3.f};
		layer.EmissiveTint = index == 0 ? core::Color3{0, 4, 0} : core::Color3{4, 0, 0};
		const auto foreground = capture(core::Name("image-export-pipeline"), core::Name("image-export"));
		const auto centre = colourAt(foreground, 16 * 33 + 16);
		REQUIRE(std::abs(centre.a - float(opacity) / 255) < .001f);
		render::PortalImageReply reply;
		binding.Expected.RequestId++;
		reply.Key = binding.Expected;
		reply.Scope = binding.ExpectedScope;
		reply.Status = render::PortalImageStatus::Ok;
		reply.Width = background.Width;
		reply.Height = background.Height;
		reply.RowStride = background.RowStride;
		reply.Pixels = background.Pixels;
		reply.Depth = background.Depth;
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
		view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
		REQUIRE(view.EyeImage != 0);
		auto composed = capture(composition, core::Name("export"));
		std::vector<glm::vec4> expectedPixels(33 * 33), actualPixels(33 * 33);
		for (size_t pixel = 0; pixel < 33 * 33; ++pixel) {
			const auto front = colourAt(foreground, pixel), back = colourAt(background, pixel);
			const float distance = depthAt(foreground, pixel);
			// The independent PBR capture clears depth to the far plane;
			// the compositing graph exports that empty sample as zero.
			const bool visible = distance > 0 && distance < view.Camera.FarPlane &&
								 (opaqueDepth == 0 || distance < opaqueDepth);
			const float alpha = visible ? front.a : 0;
			const glm::vec4 expected(
				glm::vec3(front) * alpha + glm::vec3(back) * (1 - alpha), alpha + back.a * (1 - alpha)
			);
			const auto actual = colourAt(composed, pixel);
			expectedPixels[pixel] = expected;
			actualPixels[pixel] = actual;
			for (int channel = 0; channel < 4; ++channel)
				CHECK(std::abs(actual[channel] - expected[channel]) < .004f);
			CHECK(depthAt(composed, pixel) == opaqueDepth);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR");
			output && opacity == 128 && index == 1 && backgroundOpacity == 1) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const auto stem = "transparent-depth-" + std::to_string(opaqueDepth);
			for (const bool expected : {true, false}) {
				const auto &samples = expected ? expectedPixels : actualPixels;
				const render::test::ImageView preview{
					33, 33, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
				};
				render::test::WriteImagePreview(
					directory / (stem + (expected ? "-expected.ppm" : "-actual.ppm")), preview
				);
			}
		}
		background = std::move(composed);
	}
	REQUIRE(renderer.DropPortalImage(view.EyeImage));
}

TEST_CASE(
	"peeled glass layers match direct transparent drawing and expose overflow",
	"[render][gpu][resourceimage][transparent-layer][.]"
) {
	using namespace graph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	PipelineDocument document;
	const auto worldDocument = DefaultWorldHdrDocument();
	for (const auto &edit : worldDocument.Edits()) {
		if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("present")) break;
		document.Record(edit);
	}
	const auto resource =
		[&](const std::string &name, ResourceFormat format, ResourceKind kind = ResourceKind::Colour) {
			document.Record(
				{.Kind = EditKind::AddResource, .Name = core::Name(name), .Resource = kind, .Format = format}
			);
		};
	const auto node = [&](const std::string &name, const char *kind, NodeScope scope = NodeScope::View) {
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name(name),
			 .NodeKind = core::Name(kind),
			 .Scope = scope}
		);
	};
	const auto edge = [&](EditKind kind, const std::string &name, const char *port) {
		document.Record({.Kind = kind, .Target = core::Name(name), .Key = core::Name(port)});
	};
	for (int layer = 0; layer < 3; ++layer) {
		const auto name = "glass-" + std::to_string(layer);
		resource(name, ResourceFormat::RGBA16F);
		resource(name + "-depth", ResourceFormat::R32F);
		resource(name + "-z", ResourceFormat::D32F, ResourceKind::Depth);
		node(name, "transparent-layer");
		edge(EditKind::Reads, "depth", "opaque-z");
		if (layer > 0) edge(EditKind::Reads, "glass-" + std::to_string(layer - 1) + "-z", "previous-z");
		edge(EditKind::Reads, "ordered-entities", "entities");
		edge(EditKind::Reads, "view-instances", "instances");
		edge(EditKind::Reads, "shadow", "shadow");
		edge(EditKind::Writes, name, "colour");
		edge(EditKind::Writes, name + "-depth", "depth");
		edge(EditKind::Writes, name + "-z", "z");
	}
	for (int layer = 1; layer >= 0; --layer) {
		const auto name = "compose-" + std::to_string(layer);
		resource(name, ResourceFormat::RGBA16F);
		resource(name + "-depth", ResourceFormat::R32F);
		node(name, "depth-compose");
		document.Record({.Kind = EditKind::Set, .Key = core::Name("mode"), .Value = "premultiplied"});
		edge(EditKind::Reads, "glass-" + std::to_string(layer), "foreground");
		edge(EditKind::Reads, "glass-" + std::to_string(layer) + "-depth", "foreground-depth");
		edge(EditKind::Reads, layer == 1 ? "mirrored" : "compose-1", "background");
		edge(EditKind::Reads, layer == 1 ? "linear-depth" : "compose-1-depth", "background-depth");
		edge(EditKind::Writes, name, "colour");
		edge(EditKind::Writes, name + "-depth", "depth");
	}
	for (int layer = 0; layer < 3; ++layer) {
		const auto name = "glass-" + std::to_string(layer);
		node(name + "-export", "capture", NodeScope::Frame);
		edge(EditKind::Reads, name, "source");
		edge(EditKind::Reads, name + "-depth", "depth");
	}
	for (const bool direct : {true, false}) {
		node(direct ? "direct-export" : "composed-export", "capture", NodeScope::Frame);
		edge(EditKind::Reads, direct ? "display" : "compose-0", "source");
	}
	node("room-export", "capture", NodeScope::Frame);
	edge(EditKind::Reads, "lit", "source");
	edge(EditKind::Reads, "linear-depth", "depth");
	node("native-lens-export", "capture", NodeScope::Frame);
	edge(EditKind::Reads, "lens-b", "source");
	edge(EditKind::Reads, "linear-depth", "depth");
	RenderGraph graph;
	core::Name offender;
	REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
	const core::Name pipeline("glass-peeling");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	const core::Name mesh("glass-plane"), emission("glass-white");
	REQUIRE(renderer.AddMesh(mesh, plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(emission, white));
	const int scenario = GENERATE(0, 1, 2, 3, 4, 5);
	const bool rotated = GENERATE(false, true);
	const bool offsetCoplanar = scenario == 3 ? GENERATE(false, true) : false;
	CAPTURE(scenario, rotated, offsetCoplanar);
	INFO(
		(scenario == 5
			 ? "intersecting panes: centre=(0,0,-3), half extent=(1,1), red yaw=+.35, green yaw=-.35 radians"
			 : "parallel panes")
	);
	std::vector<scene::DrawInstance> rows;
	for (int index = 0; index < (scenario == 2 ? 3 : 2); ++index) {
		scene::DrawInstance row;
		row.Source = index + 1;
		row.Mesh = mesh;
		row.HalfExtent = {1, 1, 1};
		row.Frame.Position = {0, 0, -3.f - index};
		row.Transparency = .5f;
		row.CastShadow = false;
		row.Tint = {};
		row.EmissiveMap = emission;
		row.EmissiveStrength = 4;
		row.EmissiveTint = index == 0	? core::Color3{1, 0, 0}
						   : index == 1 ? core::Color3{0, 1, 0}
										: core::Color3{0, 0, 1};
		rows.push_back(row);
	}
	if (scenario == 3) {
		rows.back().Frame.Position.Z = -3;
		if (offsetCoplanar) {
			rows.back().Frame.Position.X = .25f;
			rows.back().Frame.Position.Y = .125f;
		}
	}
	if (scenario == 5) {
		INFO("intersecting panes: centre=(0,0,-3), half extent=(1,1), red yaw=+.35, green yaw=-.35 radians");
		rows[0].Frame = core::CFrame(core::Vector3{0, 0, -3}) * core::CFrame::Angles(0, .35f, 0);
		rows[1].Frame = core::CFrame(core::Vector3{0, 0, -3}) * core::CFrame::Angles(0, -.35f, 0);
	}
	if (scenario == 1 || scenario == 4) {
		auto body = rows.front();
		body.Source = 9;
		body.Frame.Position.Z = scenario == 4 ? -3.f : -3.5f;
		body.Transparency = 0;
		body.EmissiveTint = {0, 0, 1};
		rows.push_back(body);
	}
	render::SceneTarget target{33, 33};
	render::View view;
	view.Pipeline = pipeline;
	view.WorldName = core::Name("glass-world");
	view.Target = &target;
	if (rotated) {
		view.CameraFrame = core::CFrame(core::Vector3{7, 2, 3}) * core::CFrame::Angles(-.2f, .37f, .1f);
		for (auto &row : rows)
			row.Frame = view.CameraFrame * row.Frame;
	}
	view.Instances = rows;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	const std::array<uint64_t, 4> tokens{100, 101, 102, 103};
	std::array<render::ResourceImageRequest, 4> requests;
	for (size_t layer = 0; layer < 3; ++layer)
		requests[layer] = {tokens[layer], pipeline, core::Name("glass-" + std::to_string(layer) + "-export")};
	requests[3] = {tokens[3], pipeline, core::Name("direct-export")};
	REQUIRE(renderer.RequestResourceImages(requests));
	CHECK_FALSE(renderer.TakeResourceImages(tokens));
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("glass-2")));
	const auto captured = AwaitImageGroup(renderer, tokens);
	REQUIRE(captured.front().CaptureFrame != 0);
	for (const auto &image : captured)
		CHECK(image.CaptureFrame == captured.front().CaptureFrame);
	std::array<float, 3> centreDepths;
	for (size_t layer = 0; layer < 3; ++layer) {
		core::ByteReader depth(std::span(captured[layer].Depth).subspan((16 * 33 + 16) * 4, 4));
		centreDepths[layer] = depth.ReadFloat();
	}
	const float alpha = 128.f / 255;
	float transmittance = 1;
	for (size_t layer = 0; layer < 3; ++layer) {
		const float expected = scenario == 4								  ? 0.f
							   : layer == 0									  ? 3.f
							   : layer == 1 && scenario != 1 && scenario != 3 ? 4.f
							   : layer == 2 && scenario == 2				  ? 5.f
																			  : 0.f;
		if (((scenario == 3 && offsetCoplanar) || scenario == 5) && layer == 1) {
			// Coplanar faces and the intersection centre can round to adjacent depths.
			// They may occupy two groups, but neither colour nor opacity may disappear.
			CHECK((centreDepths[layer] == 0 || std::abs(centreDepths[layer] - 3) < .001f));
		} else
			CHECK(std::abs(centreDepths[layer] - expected) < .001f);
		core::ByteReader colour(std::span(captured[layer].Pixels).subspan((16 * 33 + 16) * 8, 8));
		const auto rg = glm::unpackHalf2x16(colour.ReadUInt32());
		const auto ba = glm::unpackHalf2x16(colour.ReadUInt32());
		transmittance *= 1 - ba.y;
		if (centreDepths[layer] != 0) {
			const float expectedAlpha = (scenario == 3 || scenario == 5) && layer == 0 && centreDepths[1] == 0
											? 1 - (1 - alpha) * (1 - alpha)
											: alpha;
			CHECK(std::abs(ba.y - expectedAlpha) < .001f);
			CHECK(
				((scenario == 3 || scenario == 5) ? std::max(rg.x, rg.y)
				 : layer == 0					  ? rg.x
				 : layer == 1					  ? rg.y
												  : ba.x) > 1.9f
			);
		} else {
			CHECK(rg == glm::vec2(0));
			CHECK(ba == glm::vec2(0));
		}
	}
	if (scenario == 5) {
		// Samples straddle the intersection, with a physical depth separation
		// well above float rounding. The nearest retained colour must reverse.
		for (const uint32_t x : {14u, 18u}) {
			CAPTURE(x);
			const size_t pixel = 16 * 33 + x;
			core::ByteReader nearest(std::span(captured[0].Pixels).subspan(pixel * 8, 8));
			const auto redGreen = glm::unpackHalf2x16(nearest.ReadUInt32());
			CHECK((x < 16 ? redGreen.x : redGreen.y) > 1.9f);
			CHECK((x < 16 ? redGreen.y : redGreen.x) == 0);
			core::ByteReader front(std::span(captured[0].Depth).subspan(pixel * 4, 4));
			core::ByteReader back(std::span(captured[1].Depth).subspan(pixel * 4, 4));
			const float nearDepth = front.ReadFloat(), farDepth = back.ReadFloat();
			CHECK(nearDepth < 2.95f);
			CHECK(farDepth > 3.05f);
			core::ByteReader overflow(std::span(captured[2].Depth).subspan(pixel * 4, 4));
			CHECK(overflow.ReadFloat() == 0);
		}
	}
	const int visiblePanes = scenario == 4 ? 0 : scenario == 1 ? 1 : scenario == 2 ? 3 : 2;
	CHECK(std::abs((1 - transmittance) - (1 - std::pow(1 - alpha, visiblePanes))) < .001f);
	if (scenario != 2) {
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("composed-export"));
		REQUIRE(token != 0);
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("compose-0")));
		const auto composed = AwaitImage(renderer, token);
		core::ByteReader expected(captured[3].Pixels), actual(composed.Pixels);
		std::vector<float> directPixels, composedPixels;
		const bool perPixelOrder = scenario == 5 || (scenario == 3 && rotated && offsetCoplanar);
		size_t reorderedPixels = 0;
		while (!expected.AtEnd()) {
			const auto directRedGreen = glm::unpackHalf2x16(expected.ReadUInt32());
			const auto directBlueAlpha = glm::unpackHalf2x16(expected.ReadUInt32());
			const auto layeredRedGreen = glm::unpackHalf2x16(actual.ReadUInt32());
			const auto layeredBlueAlpha = glm::unpackHalf2x16(actual.ReadUInt32());
			directPixels.insert(
				directPixels.end(), {directRedGreen.x, directRedGreen.y, directBlueAlpha.x, directBlueAlpha.y}
			);
			composedPixels.insert(
				composedPixels.end(),
				{layeredRedGreen.x, layeredRedGreen.y, layeredBlueAlpha.x, layeredBlueAlpha.y}
			);
			if (perPixelOrder) {
				// Direct transparency has one object order. Intersecting and raster-drift coplanar
				// panes need per-pixel ordering, so compare the complete contributions independent
				// of which pane is in front at a pixel.
				CHECK(
					std::abs(
						std::min(directRedGreen.x, directRedGreen.y) -
						std::min(layeredRedGreen.x, layeredRedGreen.y)
					) < .004f
				);
				CHECK(
					std::abs(
						std::max(directRedGreen.x, directRedGreen.y) -
						std::max(layeredRedGreen.x, layeredRedGreen.y)
					) < .004f
				);
				if (std::abs(directRedGreen.x - layeredRedGreen.x) >= .004f ||
					std::abs(directRedGreen.y - layeredRedGreen.y) >= .004f)
					++reorderedPixels;
			} else {
				CHECK(std::abs(directRedGreen.x - layeredRedGreen.x) < .004f);
				CHECK(std::abs(directRedGreen.y - layeredRedGreen.y) < .004f);
			}
			CHECK(std::abs(directBlueAlpha.x - layeredBlueAlpha.x) < .004f);
			CHECK(std::abs(directBlueAlpha.y - layeredBlueAlpha.y) < .004f);
		}
		if (perPixelOrder) {
			CHECK(reorderedPixels > 0);
			core::ByteReader overflow(captured[2].Depth);
			while (!overflow.AtEnd())
				CHECK(overflow.ReadFloat() == 0);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const auto stem = "peeled-glass-" + std::to_string(scenario) + (rotated ? "-rotated" : "-front") +
							  (offsetCoplanar ? "-offset" : "");
			for (const bool direct : {true, false}) {
				const auto &samples = direct ? directPixels : composedPixels;
				const render::test::ImageView preview{
					33, 33, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
				};
				render::test::WriteImagePreview(
					directory / (stem + (direct ? "-direct.ppm" : "-composed.ppm")), preview
				);
			}
		}
	}
	if (scenario == 0) {
		const auto savedView = view;
		render::PortalCaptureLighting captureLighting;
		std::array<render::SceneLight, render::MAX_PORTAL_CAPTURE_LIGHTS> captureLights{};
		REQUIRE(render::ResolvePortalCaptureLighting(captureLighting, view, captureLights));
		auto roomRows = rows;
		auto wall = rows.front();
		wall.Source = 20;
		wall.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -6});
		wall.HalfExtent = {20, 20, 1};
		wall.Transparency = 0;
		wall.EmissiveTint = {0, 0, 1};
		wall.EmissiveStrength = 2;
		roomRows.push_back(wall);
		view.Instances = roomRows;
		view.Damage = {.Scene = true, .Objects = true, .Environment = true, .Portals = true};
		const std::array roomNodes{
			core::Name("room-export"), core::Name("glass-0-export"), core::Name("glass-1-export")
		};
		std::array<uint64_t, 3> roomTokens{};
		REQUIRE(renderer.QueueResourceImages(
			pipeline, roomNodes, view.Slot, render::ResourceImageDelivery::CopiedPixels, roomTokens
		));
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("room-export")));
		auto roomImages = AwaitImageGroup(renderer, roomTokens);
		render::PortalImageLayerSet roomLayers;
		for (size_t layer = 0; layer < roomImages.size(); ++layer) {
			auto &reply = layer == 0 ? roomLayers.Opaque : roomLayers.Transparent.emplace_back();
			auto &image = roomImages[layer];
			REQUIRE(image.Status == render::ResourceImageStatus::Ok);
			CHECK(image.CaptureFrame == roomImages.front().CaptureFrame);
			reply.Key = {1, "native-glass-lens", 1, 1};
			reply.Scope = render::PortalImageScope::OpaqueLighting;
			reply.Status = render::PortalImageStatus::Ok;
			reply.CaptureTick = reply.ContentRevision = reply.LightingRevision = 1;
			reply.Width = image.Width;
			reply.Height = image.Height;
			reply.RowStride = image.RowStride;
			reply.Pixels = std::move(image.Pixels);
			reply.Depth = std::move(image.Depth);
			reply.PixelHash = assets::Hasher::Of(reply.Pixels);
			reply.DepthHash = assets::Hasher::Of(reply.Depth);
			reply.CaptureLighting = captureLighting;
		}
		render::PortalImageCapture accepted;
		accepted.Producer.World = "native-glass-lens-owner";
		accepted.Width = accepted.Height = 33;
		accepted.CaptureLighting = captureLighting;
		const auto rotation = view.CameraFrame.Rotation();
		accepted.Camera.Position = {
			view.CameraFrame.Position.X, view.CameraFrame.Position.Y, view.CameraFrame.Position.Z
		};
		accepted.Camera.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
		const float near = view.Camera.NearPlane;
		const float half = near * std::tan(view.Camera.FieldOfViewRadians / 2);
		accepted.Camera.Frustum = {-half, half, -half, half, near, view.Camera.FarPlane};
		accepted.Binding.World = view.World;
		accepted.Binding.WorldName = view.WorldName;
		accepted.Binding.ViewSlot = view.Slot;
		accepted.Binding.Portal = core::Name("native-glass-lens");
		accepted.Binding.Expected = roomLayers.Opaque.Key;
		accepted.Binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
		accepted.Binding.ExpectedProjection = render::PortalImageProjection::Eye;
		accepted.Binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 1).ViewProjection;
		std::array<uint64_t, 3> roomHandles{};
		REQUIRE(renderer.QueuePortalImageLayerSet(accepted.Binding, std::move(roomLayers), roomHandles));
		accepted.Image = roomHandles[0];
		accepted.TransparentImages = {roomHandles[1], roomHandles[2]};
		const auto uploadFence = renderer.QueueResourceImage(pipeline, core::Name("room-export"));
		REQUIRE(uploadFence != 0);
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(AwaitImage(renderer, uploadFence).Status == render::ResourceImageStatus::Ok);
		REQUIRE(renderer.PortalImageLayerSetReady(accepted.Image));

		const core::Name owner(accepted.Producer.World), shift("native-lens-shift"), turn("native-lens-turn");
		render::ShaderCompiler compiler;
		for (const auto shader : {shift, turn}) {
			const std::string source = std::string(R"glsl(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 colour;
layout(set=2,binding=0) uniform sampler2D sceneColour;
layout(set=2,binding=1) uniform sampler2D sceneDepth;
struct Lens {vec4 centre;vec4 x;vec4 y;vec4 z;vec4 spin;};
layout(set=3,binding=0) uniform LensPass {
 mat4 vp;mat4 inverseVp;vec4 target;vec4 eye;vec4 timeCount;Lens lenses[16];vec4 cameraDepth;
} pass;
void main(){ivec2 size=textureSize(sceneColour,0);ivec2 pixel=ivec2(gl_FragCoord.xy);
)glsl") +
									   (shader == shift ? "pixel.x=(pixel.x+int(pass.timeCount.x))%size.x;"
														: "pixel=ivec2(pixel.y,size.x-1-pixel.x);") +
									   "colour=texelFetch(sceneColour,pixel,0);}";
			const auto compiled = compiler.Compile(source, render::ShaderStage::Fragment);
			REQUIRE_FALSE(compiled.Failed);
			REQUIRE(renderer.AddLensShader(shader, compiled.SpirV, owner));
			accepted.Lenses.Programs.push_back(
				{.Hash = renderer.LensShaderHash(shader, owner), .SpirV = compiled.SpirV}
			);
		}
		accepted.Lenses.TimeSeconds = 3;
		for (const auto shader : {shift, turn, shift}) {
			render::PortalCaptureLens lens;
			lens.Position = accepted.Camera.Position;
			lens.Shader = shader.Text();
			lens.ProgramHash = renderer.LensShaderHash(shader, owner);
			lens.Radius = 100;
			lens.Strength = 1;
			accepted.Lenses.Entries.push_back(lens);
			view.Lighting.ShaderLenses[view.Lighting.ShaderLensCount++] = {
				.Frame = core::CFrame(view.CameraFrame.Position),
				.Shader = shader,
				.Radius = 100,
				.Strength = 1
			};
		}
		view.LensTimeSeconds = accepted.Lenses.TimeSeconds;
		view.LensContentOwner = owner;
		accepted.LensPrograms = renderer.RetainPortalLensPrograms(accepted.Lenses);
		REQUIRE(accepted.LensPrograms != 0);
		const auto retainedAgain = renderer.RetainPortalLensPrograms(accepted.Lenses);
		REQUIRE(retainedAgain == accepted.LensPrograms);
		renderer.ReleasePortalLensPrograms(retainedAgain);
		if (!rotated) {
			for (int invalidKind = 0; invalidKind < 4; ++invalidKind) {
				CAPTURE(invalidKind);
				auto invalid = accepted.Lenses;
				auto &program = invalid.Programs.front();
				const auto replacedHash = program.Hash;
				if (invalidKind == 0) {
					REQUIRE(program.SpirV.size() > 5);
					program.SpirV[5] = 0;
				} else {
					const auto source =
						invalidKind == 1
							? std::string("#version 450\nvoid main(){gl_Position=vec4(0,0,0,1);}")
							: std::string("#version 450\nlayout(location=0) out vec4 colour;\nlayout(set=") +
								  (invalidKind == 2 ? "0,binding=0" : "2,binding=2") +
								  ") uniform sampler2D image;\nvoid main(){colour=texture(image,vec2(.5));}";
					const auto module = compiler.Compile(
						source, invalidKind == 1 ? render::ShaderStage::Vertex : render::ShaderStage::Fragment
					);
					REQUIRE_FALSE(module.Failed);
					program.SpirV = module.SpirV;
				}
				program.Hash = assets::Hasher::Of(std::as_bytes(std::span(program.SpirV)));
				for (auto &lens : invalid.Entries)
					if (lens.ProgramHash == replacedHash) lens.ProgramHash = program.Hash;
				REQUIRE(render::ValidPortalCaptureLenses(invalid));
				CHECK(renderer.RetainPortalLensPrograms(invalid) == 0);
				const auto retainedValid = renderer.RetainPortalLensPrograms(accepted.Lenses);
				REQUIRE(retainedValid == accepted.LensPrograms);
				renderer.ReleasePortalLensPrograms(retainedValid);
			}
		}
		const core::Name collidingOwner("portal.lens.programs/0");
		const auto collisionProgram = compiler.Compile(
			"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(16,0,16,1);}",
			render::ShaderStage::Fragment
		);
		REQUIRE_FALSE(collisionProgram.Failed);
		const auto collisionHash = assets::Hasher::Of(std::as_bytes(std::span(collisionProgram.SpirV)));
		for (const auto shader : {shift, turn})
			REQUIRE(renderer.AddLensShader(shader, collisionProgram.SpirV, collidingOwner));
		auto body = wall;
		body.Source = 21;
		body.HalfExtent = {.55f, .7f, 1};
		body.EmissiveTint = {1, 1, 0};
		body.EmissiveStrength = 4;
		for (const float bodyDistance : {2.f, 3.5f, 5.f}) {
			CAPTURE(bodyDistance);
			body.Frame = view.CameraFrame * core::CFrame(core::Vector3{.2f, -.1f, -bodyDistance});
			auto nativeRows = roomRows;
			nativeRows.push_back(body);
			view.Instances = nativeRows;
			view.Damage = {.Scene = true, .Objects = true, .Environment = true, .Portals = true};
			const std::array nativeNodes{core::Name("native-lens-export"), core::Name("direct-export")};
			std::array<uint64_t, 2> nativeTokens{};
			REQUIRE(renderer.QueueResourceImages(
				pipeline, nativeNodes, view.Slot, render::ResourceImageDelivery::CopiedPixels, nativeTokens
			));
			REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false)
						.Ran(core::Name("native-lens-export")));
			const auto native = AwaitImageGroup(renderer, nativeTokens);
			REQUIRE(native[0].Status == render::ResourceImageStatus::Ok);
			REQUIRE(native[1].Status == render::ResourceImageStatus::Ok);
			CHECK(native[0].Pixels != native[1].Pixels);
			for (size_t pixel = 0; pixel < 33 * 33; ++pixel) {
				const size_t sampledX = (pixel / 33 + 3) % 33;
				const size_t sampledY = 32 - (pixel % 33 + 3) % 33;
				const auto expected = std::span(native[1].Pixels).subspan((sampledY * 33 + sampledX) * 8, 8);
				const auto actual = std::span(native[0].Pixels).subspan(pixel * 8, 8);
				CHECK(std::equal(actual.begin(), actual.end(), expected.begin()));
			}
			view.Instances = std::span(&body, 1);
			if (bodyDistance == 2 && !rotated) {
				auto mismatched = accepted;
				for (auto &lens : mismatched.Lenses.Entries)
					lens.ProgramHash = lens.Shader == shift.Text() ? accepted.Lenses.Programs[1].Hash
																   : accepted.Lenses.Programs[0].Hash;
				REQUIRE(render::ValidPortalCaptureLenses(mismatched.Lenses));
				CHECK(renderer.ComposePortalBodyImage(mismatched, view) == 0);
			}
			const auto prepared = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(prepared != 0);
			REQUIRE(renderer.DropPortalImage(prepared));
			const auto copiedToken = renderer.QueueResourceImage(
				core::Name("portal-body-eye-image/lenses/layers-2/0"), core::Name("export")
			);
			REQUIRE(copiedToken != 0);
			const auto copiedHandle = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(copiedHandle != 0);
			const auto copied = AwaitImage(renderer, copiedToken);
			REQUIRE(copied.Status == render::ResourceImageStatus::Ok);
			REQUIRE(copied.Pixels.size() == native[0].Pixels.size());
			core::ByteReader nativePixels(native[0].Pixels), copiedPixels(copied.Pixels);
			while (!nativePixels.AtEnd()) {
				const auto direct = glm::unpackHalf2x16(nativePixels.ReadUInt32());
				const auto imported = glm::unpackHalf2x16(copiedPixels.ReadUInt32());
				// The two glass stores have the same bound as the direct peel comparison above.
				CHECK(std::abs(direct.x - imported.x) < .004f);
				CHECK(std::abs(direct.y - imported.y) < .004f);
			}
			REQUIRE(copied.Depth.size() == native[0].Depth.size());
			core::ByteReader nativeDepth(native[0].Depth), copiedDepth(copied.Depth);
			while (!nativeDepth.AtEnd())
				CHECK(std::abs(nativeDepth.ReadFloat() - copiedDepth.ReadFloat()) < .003f);
			REQUIRE(renderer.DropPortalImage(copiedHandle));
			if (bodyDistance == 2) {
				renderer.DropContentOwner(collidingOwner);
				for (const auto shader : {shift, turn})
					CHECK_FALSE(renderer.HasLensShader(shader, collidingOwner));
				const auto afterDropToken = renderer.QueueResourceImage(
					core::Name("portal-body-eye-image/lenses/layers-2/0"), core::Name("export")
				);
				REQUIRE(afterDropToken != 0);
				const auto afterDropHandle = renderer.ComposePortalBodyImage(accepted, view);
				REQUIRE(afterDropHandle != 0);
				const auto afterDrop = AwaitImage(renderer, afterDropToken);
				REQUIRE(afterDrop.Status == render::ResourceImageStatus::Ok);
				CHECK(afterDrop.Pixels == copied.Pixels);
				CHECK(afterDrop.Depth == copied.Depth);
				REQUIRE(renderer.DropPortalImage(afterDropHandle));
				for (const auto shader : {shift, turn})
					REQUIRE(renderer.AddLensShader(shader, collisionProgram.SpirV, collidingOwner));
			}
		}
		renderer.ReleasePortalLensPrograms(accepted.LensPrograms);
		for (const auto shader : {shift, turn}) {
			CHECK(renderer.HasLensShader(shader, collidingOwner));
			CHECK(renderer.LensShaderHash(shader, collidingOwner) == collisionHash);
		}
		renderer.DropContentOwner(collidingOwner);
		CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
		REQUIRE(renderer.DropPortalImage(accepted.Image));
		view = savedView;
	}
	if (scenario == 0 && !rotated) {
		rows.front().Shader = core::Name("uncaptured-material");
		const core::Name unrelated("unrelated-glass-pipeline");
		REQUIRE(renderer.SetPipeline(unrelated, graph));
		const auto waiting = renderer.QueueResourceImage(unrelated, core::Name("glass-0-export"));
		REQUIRE(waiting != 0);
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("glass-0-export"));
		REQUIRE(token != 0);
		CHECK_FALSE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("glass-0")));
		CHECK(AwaitImage(renderer, token).Status == render::ResourceImageStatus::Failed);
		CHECK_FALSE(renderer.TakeResourceImage(waiting).has_value());
		rows.front().Shader = {};
		view.Pipeline = unrelated;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("glass-0")));
		CHECK(AwaitImage(renderer, waiting).Status == render::ResourceImageStatus::Ok);
	}
}

TEST_CASE(
	"oblique portal captures retain a depth-tested floor canvas",
	"[render][gpu][resourceimage][portal-floor-canvas][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lens-b", true);
	gui::RegisterGuiClasses();
	ecs::Store world("floor-canvas-capture");
	const auto collector = world.CreateInstance(gui::GuiClass("SurfaceGui"), "FloorCanvas");
	gui::SpatialCanvas canvas;
	canvas.Size = {100, 100};
	canvas.Origin = {-50, .001f, -50};
	canvas.AxisX = {100, 0, 0};
	canvas.AxisY = {0, 0, 100};
	canvas.Normal = {0, 1, 0};
	world.Set(collector, canvas);
	gui::DrawCommand paint;
	paint.Collector = collector;
	paint.Spatial = true;
	paint.Bounds = {{0, 0}, {100, 100}};
	paint.Clip = paint.Bounds;
	paint.Tint = {0, 0, 1};
	gui::DrawList list;
	render::InterfacePass interface;
	REQUIRE(interface.Initialise(renderer.Backend().Device, renderer.Backend().ColourFormat));
	const bool authored = GENERATE(false, true);
	CAPTURE(authored);
	if (authored) {
		render::ShaderCompiler compiler;
		const auto compiled = compiler.Compile(
			"#version 450\nlayout(location=0) out vec4 colour;\n"
			"void main(){colour=vec4(0,0,1,1);}",
			render::ShaderStage::Fragment
		);
		REQUIRE_FALSE(compiled.Failed);
		paint.Shader = core::Name("FloorBlue");
		REQUIRE(interface.AddShaderVariant(paint.Shader, compiled.SpirV));
	}
	list.Commands.push_back(paint);
	interface.Submit(list, {128, 128}, {128, 128}, world);
	scene::DrawInstance floor;
	floor.Source = 1;
	floor.Frame.Position = {0, -1, 0};
	floor.HalfExtent = {50, 1, 50};
	render::SceneTarget target{128, 128};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.World = 1;
	view.WorldName = core::Name("floor-canvas-capture");
	view.Target = &target;
	view.Instances = std::span(&floor, 1);
	const bool oblique = GENERATE(false, true);
	CAPTURE(oblique);
	const int seamPose = GENERATE(0, 1, 2);
	CAPTURE(seamPose);
	render::PortalCaptureCamera camera;
	camera.Position = {-1.3462248f, 3.9990549f, -3.1353846f};
	camera.Orientation = {0, .14647098f, 0, -.98921496f};
	camera.Frustum = {-.013879966f, .013879966f, -.013879966f, .013879966f, .019859195f, 500};
	if (oblique) camera.ClipPlane = {-8.7422777e-8f, 0, -1, -3.1552439f};
	// Retain capture poses from product frames that lost the floor near the seam.
	if (seamPose == 1) {
		camera.Position = {-2.0254967f, 4.0006418f, -3.0771480f};
		camera.Orientation = {0, .18787664f, 0, -.98219270f};
		camera.Frustum = {-.034231212f, .034231212f, -.034231212f, .034231212f, .048977375f, 500};
		if (oblique) camera.ClipPlane = {-8.7422777e-8f, 0, -1, -3.1261256f};
	}
	if (seamPose == 2) {
		camera.Position = {-1.9555125f, 4.0011396f, -3.1463609f};
		camera.Orientation = {0, .19474730f, 0, -.98085344f};
		camera.Frustum = {-.010044106f, .010044106f, -.010044106f, .010044106f, .014370918f, 500};
		if (oblique) camera.ClipPlane = {-8.7422777e-8f, 0, -1, -3.1607320f};
	}
	REQUIRE(
		render::ResolvePortalCaptureCamera(
			camera, oblique ? render::PortalImageProjection::Seam : render::PortalImageProjection::Eye, view
		)
	);
	render::OverlayImage overlay;
	const auto capture = [&](render::FrameOverlayHook *hook) {
		const auto token = renderer.QueueResourceImage(view.Pipeline, core::Name("image-export"));
		REQUIRE(token != 0);
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, hook, false).Ran(core::Name("image-export")));
		return AwaitImage(renderer, token);
	};
	const auto bare = capture(nullptr);
	const auto painted = capture(&interface);
	REQUIRE(interface.LastBatchCount() > 0);
	const auto pixel = [](const render::ResourceImage &image, uint32_t x, uint32_t y) {
		core::ByteReader bytes(std::span(image.Pixels).subspan(size_t(y) * image.RowStride + x * 8, 8));
		const auto rg = glm::unpackHalf2x16(bytes.ReadUInt32());
		const auto ba = glm::unpackHalf2x16(bytes.ReadUInt32());
		return glm::vec4(rg.x, rg.y, ba.x, ba.y);
	};
	for (uint32_t x : {16u, 48u, 80u, 112u}) {
		const auto before = pixel(bare, x, 126), after = pixel(painted, x, 126);
		CAPTURE(x, before.r, before.g, before.b, after.r, after.g, after.b);
		REQUIRE(before.r > .1f);
		CHECK(after.b > .9f);
		CHECK(after.r < .01f);
		CHECK(after.g < .01f);
	}
	const auto matrices = view.Projection ? scene::ResolveSurfaceCamera(view.CameraFrame, *view.Projection)
										  : scene::ResolveCamera(view.CameraFrame, view.Camera, 1);
	const glm::dmat4 projection(matrices.ViewProjection), inverse = glm::inverse(projection);
	const glm::dvec3 eye(
		view.CameraFrame.Position.X, view.CameraFrame.Position.Y, view.CameraFrame.Position.Z
	);
	for (const float top : {.011f, .3f}) {
		std::array occluded{floor, floor};
		occluded[1].Source = 2;
		occluded[1].Frame.Position.Y = top - .1f;
		occluded[1].HalfExtent.Y = .1f;
		occluded[1].Tint = {1, 0, 0};
		view.Instances = occluded;
		const auto blocker = capture(nullptr);
		const auto hidden = capture(&interface);
		for (uint32_t x : {16u, 48u, 80u, 112u}) {
			const auto visible = pixel(blocker, x, 126);
			core::ByteReader floorDepth(std::span(bare.Depth).subspan((126 * bare.Width + x) * 4, 4));
			core::ByteReader blockerDepth(std::span(blocker.Depth).subspan((126 * blocker.Width + x) * 4, 4));
			const float floorDistance = floorDepth.ReadFloat(), blockerDistance = blockerDepth.ReadFloat();
			const glm::dvec4 near =
				inverse * glm::dvec4(2 * (double(x) + .5) / 128 - 1, 1 - 2 * 126.5 / 128, 0, 1);
			const glm::dvec3 ray = glm::dvec3(near) / near.w - eye;
			const auto rawAt = [&](double height) {
				const glm::dvec3 position = eye + ray * ((height - eye.y) / ray.y);
				const glm::dvec4 clip = projection * glm::dvec4(position, 1);
				return clip.z / clip.w;
			};
			const double rawGap = rawAt(.001) - rawAt(top);
			CAPTURE(top, x, visible.r, visible.g, visible.b, floorDistance, blockerDistance, rawGap);
			REQUIRE(rawGap > 1.0 / 524288.0);
			REQUIRE(visible.r > visible.b * 2);
			REQUIRE(blockerDistance < floorDistance - .001f);
			const auto colour = pixel(hidden, x, 126);
			CAPTURE(colour.r, colour.g, colour.b);
			CHECK(colour.r > colour.b * 2);
		}
	}
}

TEST_CASE(
	"copied portal layers place the current body between glass and room",
	"[render][gpu][resourceimage][layer-body][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const core::Name lensOwner("copied-lens-owner");
	const core::Name lensA("copied-lens-a"), lensB("copied-lens-b");
	render::ShaderCompiler lensCompiler;
	const auto lensSource = [](bool second) {
		return std::string(R"glsl(#version 450
layout(location=0) in vec2 inUv;
layout(location=0) out vec4 outColour;
layout(set=2,binding=0) uniform sampler2D sceneColour;
layout(set=2,binding=1) uniform sampler2D linearDepth;
struct Lens { vec4 CentreRadius; vec4 AxisXInner; vec4 AxisYFalloff; vec4 AxisZStrength; vec4 SpinPriority; };
layout(set=3,binding=0) uniform LensPass {
 mat4 ViewProjection; mat4 InverseViewProjection; vec4 Target; vec4 Eye; vec4 TimeCount; Lens Lenses[16]; vec4 CameraDepth;
} pass;
void main() {
 vec4 colour=texture(sceneColour,inUv);
 vec3 addition=vec3(pass.TimeCount.x, texture(linearDepth,inUv).r, pass.Lenses[0].AxisZStrength.w)/16.0;
)glsl") + (second ? "outColour=vec4(colour.rgb*2.0+addition.bgr,colour.a);}"
				  : "outColour=vec4(colour.rgb*0.5+addition,colour.a);}");
	};
	const auto firstProgram = lensCompiler.Compile(lensSource(false), render::ShaderStage::Fragment);
	const auto secondProgram = lensCompiler.Compile(lensSource(true), render::ShaderStage::Fragment);
	REQUIRE_FALSE(firstProgram.Failed);
	REQUIRE_FALSE(secondProgram.Failed);
	REQUIRE(renderer.AddLensShader(lensA, firstProgram.SpirV, lensOwner));
	REQUIRE(renderer.AddLensShader(lensB, secondProgram.SpirV, lensOwner));
	CHECK(
		renderer.LensShaderHash(lensA, lensOwner) ==
		assets::Hasher::Of(std::as_bytes(std::span(firstProgram.SpirV)))
	);
	CHECK(renderer.LensShaderHash(lensA).IsZero());

	const bool seam = GENERATE(false, true);
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPortalBodyDocument(seam, true), graph, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipeline("layer-body");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const float bodyDepth = GENERATE(1.f, 3.f, 5.f, 7.f);
	const bool rotated = GENERATE(false, true);
	CAPTURE(bodyDepth, rotated, seam);
	const auto image = [](glm::vec4 colour, float distance, uint32_t extent = 17) {
		render::PortalImageReply reply;
		reply.Key = {1, "layer-door", 1, 1};
		reply.Scope = render::PortalImageScope::OpaqueLighting;
		reply.Status = render::PortalImageStatus::Ok;
		reply.CaptureTick = reply.ContentRevision = reply.LightingRevision = 1;
		reply.Width = reply.Height = extent;
		reply.RowStride = extent * 8;
		core::ByteWriter pixels, depth;
		for (size_t pixel = 0; pixel < size_t(extent) * extent; ++pixel) {
			pixels.WriteUInt32(glm::packHalf2x16({colour.r, colour.g}));
			pixels.WriteUInt32(glm::packHalf2x16({colour.b, colour.a}));
			depth.WriteFloat(distance);
		}
		reply.Pixels.assign(pixels.Bytes().begin(), pixels.Bytes().end());
		reply.Depth.assign(depth.Bytes().begin(), depth.Bytes().end());
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
		return reply;
	};
	render::PortalImageLayerSet layers{
		image({0, 0, 2, 1}, 6), {image({0, 1, 0, .5f}, 2), image({1, 0, 1, .25f}, 4)}
	};
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(render::EncodePortalImageLayerSet(layers, wire, error));
	render::PortalImageLayerSet decoded;
	REQUIRE(render::DecodePortalImageLayerSet(wire, decoded, error));
	render::SceneTarget target{17, 17};
	render::View view;
	view.Pipeline = pipeline;
	view.Target = &target;
	view.WorldName = core::Name("layer-receiver");
	view.EyeImageKey = core::Name("layer-door");
	if (rotated)
		view.CameraFrame = core::CFrame(core::Vector3{7, 2, 3}) * core::CFrame::Angles(-.2f, .37f, .1f);
	render::PortalCaptureCamera captureCamera;
	const auto rotation = view.CameraFrame.Rotation();
	captureCamera.Position = {
		view.CameraFrame.Position.X, view.CameraFrame.Position.Y, view.CameraFrame.Position.Z
	};
	captureCamera.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
	const float near = view.Camera.NearPlane;
	const float half = near * std::tan(view.Camera.FieldOfViewRadians / 2);
	captureCamera.Frustum = {-half, half, -half, half, near, view.Camera.FarPlane};
	const auto projection = seam ? render::PortalImageProjection::Seam : render::PortalImageProjection::Eye;
	if (seam) {
		const auto normal = view.CameraFrame.VectorToWorldSpace({0, 0, -1});
		const auto mouth = view.CameraFrame.PointToWorldSpace({0, 0, -.5f});
		captureCamera.ClipPlane = {normal.X, normal.Y, normal.Z, -normal.Dot(mouth)};
	}
	REQUIRE(render::ResolvePortalCaptureCamera(captureCamera, projection, view));
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = view.EyeImageKey;
	binding.Expected = decoded.Opaque.Key;
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = projection;
	binding.Sampling = seam ? scene::ResolveSurfaceCamera(view.CameraFrame, *view.Projection).ViewProjection
							: scene::ResolveCamera(view.CameraFrame, view.Camera, 1).ViewProjection;
	std::array<uint64_t, 3> imported{};
	REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(decoded), imported));
	view.EyeImage = imported[0];
	view.EyeTransparentImages = {imported[1], imported[2]};
	CHECK_FALSE(renderer.PortalImageLayerSetReady(view.EyeImage));
	CHECK(renderer.PortalImageUsage().Images == 3);
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("body-emission"), white));
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("current-body-plane"), plane));
	scene::DrawInstance body;
	body.Mesh = core::Name("current-body-plane");
	body.Source = 1;
	body.HalfExtent = {.7f, .7f, 1};
	body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
	body.Tint = {};
	body.EmissiveMap = core::Name("body-emission");
	body.EmissiveTint = {1, 0, 0};
	body.EmissiveStrength = 16;
	body.CastShadow = false;
	view.Instances = std::span(&body, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	render::OverlayImage overlay;
	auto capture = [&]() {
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("export"));
		REQUIRE(token != 0);
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		return AwaitImage(renderer, token);
	};
	const auto result = capture();
	REQUIRE(result.Status == render::ResourceImageStatus::Ok);
	CHECK(renderer.PortalImageLayerSetReady(view.EyeImage));
	const auto checkImage = [&](const render::ResourceImage &captured, float distance) {
		const auto projection = scene::ResolveCamera(core::CFrame{}, view.Camera, 1).ViewProjection;
		const float halfWidth = std::abs(projection[0][0]) * .7f / distance;
		const float halfHeight = std::abs(projection[1][1]) * .7f / distance;
		core::ByteReader pixels(captured.Pixels), depth(captured.Depth);
		std::vector<float> actualSamples, expectedSamples;
		for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
			const float x = 2 * (float(pixel % 17) + .5f) / 17 - 1;
			const float y = 2 * (float(pixel / 17) + .5f) / 17 - 1;
			const bool bodyVisible = distance < 6 && std::abs(x) < halfWidth && std::abs(y) < halfHeight;
			const float visibleDepth = bodyVisible ? distance : 6;
			glm::vec4 expected = bodyVisible ? glm::vec4{16, 0, 0, 1} : glm::vec4{0, 0, 2, 1};
			if (visibleDepth > 4) expected = glm::vec4{1, 0, 1, .25f} + expected * .75f;
			if (visibleDepth > 2) expected = glm::vec4{0, 1, 0, .5f} + expected * .5f;
			const auto rg = glm::unpackHalf2x16(pixels.ReadUInt32());
			const auto ba = glm::unpackHalf2x16(pixels.ReadUInt32());
			const glm::vec4 actual{rg, ba};
			for (size_t channel = 0; channel < 4; ++channel) {
				CHECK(std::abs(actual[channel] - expected[channel]) < .004f);
				actualSamples.push_back(actual[channel]);
				expectedSamples.push_back(expected[channel]);
			}
			CHECK(std::abs(depth.ReadFloat() - visibleDepth) < .003f);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const auto stem =
				"layer-body-" + std::to_string(int(distance)) + (rotated ? "-rotated" : "-front");
			for (const bool reference : {true, false}) {
				const auto &samples = reference ? expectedSamples : actualSamples;
				const render::test::ImageView preview{
					17, 17, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
				};
				render::test::WriteImagePreview(
					directory / (stem + (reference ? "-expected.ppm" : "-actual.ppm")), preview
				);
			}
		}
	};
	checkImage(result, bodyDepth);
	const auto uploads = renderer.PortalImageUsage().Uploads;
	std::swap(view.EyeTransparentImages[0], view.EyeTransparentImages[1]);
	CHECK(capture().Status == render::ResourceImageStatus::Failed);
	std::swap(view.EyeTransparentImages[0], view.EyeTransparentImages[1]);
	CHECK(capture().Pixels == result.Pixels);
	const uint64_t retainedTransparent = view.EyeTransparentImages[0];
	view.EyeTransparentImages[0] = 0;
	const uint64_t refusedResident = renderer.QueueResourceImage(
		pipeline, core::Name("export"), 0, render::ResourceImageDelivery::Resident
	);
	REQUIRE(refusedResident != 0);
	const auto refused = capture();
	CHECK(refused.Status == render::ResourceImageStatus::Failed);
	CHECK(refused.Pixels.empty());
	CHECK(refused.Width == 0);
	CHECK(refused.Height == 0);
	CHECK(renderer.AdoptResourceImage(refusedResident, binding) == 0);
	CHECK(renderer.CancelResourceImage(refusedResident));
	view.EyeTransparentImages[0] = retainedTransparent;
	CHECK(capture().Pixels == result.Pixels);
	const float movedDepth = bodyDepth == 1 ? 7.f : 1.f;
	body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -movedDepth});
	const auto moved = capture();
	REQUIRE(moved.Status == render::ResourceImageStatus::Ok);
	checkImage(moved, movedDepth);
	CHECK(moved.Pixels != result.Pixels);
	CHECK(renderer.PortalImageUsage().Uploads == uploads);
	if (bodyDepth == 3 && !rotated) {
		render::PortalImageCapture accepted;
		accepted.Image = imported[0];
		accepted.TransparentImages = {imported[1], imported[2]};
		accepted.Binding = binding;
		accepted.Width = accepted.Height = 17;
		accepted.CaptureLighting.emplace();
		accepted.Camera = captureCamera;
		auto currentView = view;
		currentView.Damage = {};
		currentView.CameraFrame.Position = {100, 100, 100};
		const auto composed = renderer.ComposePortalBodyImage(accepted, currentView);
		REQUIRE(composed != 0);
		CHECK(renderer.PortalImageUsage().Images == 4);
		CHECK(renderer.PortalImageLayerSetReady(accepted.Image));
		const auto token = renderer.QueueResourceImage(
			core::Name(seam ? "portal-body-seam-image/layers-2/0" : "portal-body-eye-image/layers-2/0"),
			core::Name("export")
		);
		REQUIRE(token != 0);
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
		const auto replaced = renderer.ComposePortalBodyImage(accepted, currentView);
		REQUIRE(replaced != 0);
		CHECK(AwaitImage(renderer, token).Pixels == result.Pixels);
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -movedDepth});
		CHECK(renderer.PortalImageUsage().Images == 4);
		CHECK(renderer.PortalImageUsage().Uploads == uploads);
		view.Damage.Objects = false;
		CHECK(capture().Pixels == moved.Pixels);
		view.Damage.Objects = true;
		std::swap(accepted.TransparentImages[0], accepted.TransparentImages[1]);
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		std::swap(accepted.TransparentImages[0], accepted.TransparentImages[1]);
		const auto acceptedCamera = accepted.Camera;
		accepted.Camera.Frustum[4] = 0;
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		accepted.Camera = acceptedCamera;
		body.Transparency = .5f;
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		body.Transparency = 0;
		currentView.WorldName = core::Name("foreign-body-owner");
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		CHECK(renderer.PortalImageUsage().Images == 4);
		REQUIRE(renderer.DropPortalImage(replaced));
		CHECK(renderer.PortalImageUsage().Images == 3);
	}
	if (bodyDepth == 3 && !rotated) {
		view.WorldName = core::Name("foreign-layer-receiver");
		CHECK(capture().Status == render::ResourceImageStatus::Failed);
		view.WorldName = binding.WorldName;
		CHECK(capture().Pixels == moved.Pixels);
		auto foreign = layers.Transparent[0];
		binding.Layer = 3;
		CHECK(renderer.QueuePortalImage(binding, std::move(foreign)) == 0);
		foreign = layers.Transparent[0];
		foreign.Key.CameraRevision++;
		binding.Layer = 1;
		binding.Expected = foreign.Key;
		view.EyeTransparentImages[0] = renderer.QueuePortalImage(binding, std::move(foreign));
		REQUIRE(view.EyeTransparentImages[0] != 0);
		CHECK(capture().Status == render::ResourceImageStatus::Failed);
		CHECK(renderer.DropPortalImage(view.EyeTransparentImages[0]));
		view.EyeTransparentImages[0] = imported[1];
		CHECK(capture().Pixels == moved.Pixels);
	}

	if (bodyDepth == 3 && !rotated) {
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
		CHECK(capture().Pixels == result.Pixels);
		auto replacement = layers;
		replacement.Transparent[0] = image({1, 0, 0, .5f}, 2);
		for (auto *member : {&replacement.Opaque, &replacement.Transparent[0], &replacement.Transparent[1]})
			member->Key.RequestId = 2;
		binding.Layer = 0;
		binding.Expected = replacement.Opaque.Key;
		std::array<uint64_t, 3> next{777, 888, 999};
		auto invalid = replacement;
		invalid.Transparent[1].CaptureTick++;
		const auto preserved = invalid;
		CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(invalid), next));
		CHECK(invalid == preserved);
		CHECK(next == std::array<uint64_t, 3>{777, 888, 999});
		CHECK(renderer.PortalImageUsage().Images == 3);
		REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(replacement), next));
		CHECK_FALSE(renderer.PortalImageLayerSetReady(next[0]));
		CHECK(renderer.PortalImageLayerSetReady(imported[0]));
		CHECK(renderer.PortalImageUsage().Images == 6);
		// Upload the replacement while still drawing the old complete set.
		CHECK(capture().Pixels == result.Pixels);
		REQUIRE(renderer.PortalImageLayerSetReady(next[0]));
		view.EyeImage = next[0];
		view.EyeTransparentImages = {next[1], next[2]};
		const auto replaced = capture();
		REQUIRE(replaced.Status == render::ResourceImageStatus::Ok);
		core::ByteReader before(result.Pixels), after(replaced.Pixels);
		for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
			const auto oldRg = glm::unpackHalf2x16(before.ReadUInt32());
			const auto newRg = glm::unpackHalf2x16(after.ReadUInt32());
			CHECK(std::abs(newRg.x - oldRg.x - 1) < .004f);
			CHECK(std::abs(newRg.y - oldRg.y + 1) < .004f);
			CHECK(before.ReadUInt32() == after.ReadUInt32());
		}
		CHECK(renderer.DropPortalImage(next[2]));
		CHECK_FALSE(renderer.PortalImageLayerSetReady(next[0]));
		CHECK_FALSE(renderer.DropPortalImage(next[0]));
		view.EyeImage = imported[0];
		view.EyeTransparentImages = {imported[1], imported[2]};
		CHECK(capture().Pixels == result.Pixels);

		for (uint64_t frame = 0; frame < 6; ++frame) {
			auto cycle = layers;
			for (auto *member : {&cycle.Opaque, &cycle.Transparent[0], &cycle.Transparent[1]})
				member->Key.RequestId = 10 + frame;
			binding.Expected = cycle.Opaque.Key;
			const auto allocations = renderer.MemoryStatistics().TextureAllocations;
			REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(cycle), next));
			CHECK(renderer.MemoryStatistics().TextureAllocations == allocations);
			CHECK(capture().Pixels == result.Pixels);
			REQUIRE(renderer.PortalImageLayerSetReady(next[0]));
			view.EyeImage = next[0];
			view.EyeTransparentImages = {next[1], next[2]};
			CHECK(capture().Pixels == result.Pixels);
			CHECK(renderer.DropPortalImage(next[0]));
			view.EyeImage = imported[0];
			view.EyeTransparentImages = {imported[1], imported[2]};
			CHECK(renderer.PortalImageUsage().CachedTextureBytes == size_t(17) * 17 * 12 * 3);
		}

		// Budget the old and new sets together, both before and after upload.
		std::array<uint64_t, 10> pressure{};
		binding.Expected = layers.Opaque.Key;
		for (size_t index = 0; index < pressure.size(); ++index) {
			auto filler = image({0, 0, 2, 1}, 6, 512);
			filler.Key.PortalKey = "pressure-" + std::to_string(index);
			auto owner = binding;
			owner.Portal = core::Name(filler.Key.PortalKey);
			owner.Expected = filler.Key;
			pressure[index] = renderer.QueuePortalImage(owner, std::move(filler));
			REQUIRE(pressure[index] != 0);
		}
		render::PortalImageLayerSet large{
			image({0, 0, 2, 1}, 6, 256), {image({0, 1, 0, .5f}, 2, 256), image({1, 0, 1, .25f}, 4, 256)}
		};
		const auto retainedLarge = large;
		const auto beforeRefusal = next;
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 30 * 1024 * 1024);
		CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(large), next));
		CHECK(large == retainedLarge);
		CHECK(next == beforeRefusal);
		CHECK(capture().Pixels == result.Pixels);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		CHECK(renderer.PortalImageUsage().TextureBytes > 30 * 1024 * 1024);
		CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(large), next));
		CHECK(large == retainedLarge);
		CHECK(next == beforeRefusal);
		for (const auto handle : pressure)
			CHECK(renderer.DropPortalImage(handle));
		REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(large), next));
		CHECK_FALSE(renderer.PortalImageLayerSetReady(next[0]));
		CHECK(renderer.DropPortalImage(next[1]));
		CHECK(renderer.PortalImageUsage().Images == 3);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		CHECK(capture().Pixels == result.Pixels);
	}

	{
		binding.Layer = 0;
		binding.Expected = layers.Opaque.Key;
		const auto baseline = renderer.PortalImageUsage();
		for (int cycle = 0; cycle < 2; ++cycle) {
			auto withOverlay = layers;
			withOverlay.SpatialOverlay = image({.25f, .5f, 0, .5f}, 0);
			withOverlay.SpatialOverlay->Depth.clear();
			withOverlay.SpatialOverlay->DepthHash = {};
			const auto saved = withOverlay;
			std::array<uint64_t, 4> overlayHandles{};
			const auto allocations = renderer.MemoryStatistics().TextureAllocations;
			REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(withOverlay), overlayHandles));
			if (cycle != 0) CHECK(renderer.MemoryStatistics().TextureAllocations == allocations);
			CHECK(renderer.PortalImageUsage().Images == baseline.Images + 4);
			CHECK(renderer.PortalImageUsage().TextureBytes == baseline.TextureBytes + 17 * 17 * 44);
			CHECK_FALSE(renderer.PortalImageLayerSetReady(overlayHandles[0]));
			CHECK(capture().Status == render::ResourceImageStatus::Ok);
			CHECK(renderer.PortalImageLayerSetReady(overlayHandles[0]));
			CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
			render::PortalImageCapture accepted;
			accepted.Image = overlayHandles[0];
			accepted.TransparentImages = {overlayHandles[1], overlayHandles[2]};
			accepted.SpatialOverlayImage = overlayHandles[3];
			accepted.Binding = binding;
			accepted.Width = accepted.Height = 17;
			accepted.CaptureLighting.emplace();
			accepted.Camera = captureCamera;
			body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
			REQUIRE(renderer.ComposePortalBodyImage(accepted, view) != 0);
			const auto token = renderer.QueueResourceImage(
				core::Name(
					seam ? "portal-body-seam-image/overlay/layers-2/0"
						 : "portal-body-eye-image/overlay/layers-2/0"
				),
				core::Name("export")
			);
			REQUIRE(token != 0);
			const auto composed = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(composed != 0);
			const auto blended = AwaitImage(renderer, token);
			REQUIRE(blended.Status == render::ResourceImageStatus::Ok);
			CHECK(blended.Depth == result.Depth);
			core::ByteReader sourcePixels(result.Pixels), blendedPixels(blended.Pixels);
			for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
				const auto oldRg = glm::unpackHalf2x16(sourcePixels.ReadUInt32());
				const auto oldBa = glm::unpackHalf2x16(sourcePixels.ReadUInt32());
				const auto newRg = glm::unpackHalf2x16(blendedPixels.ReadUInt32());
				const auto newBa = glm::unpackHalf2x16(blendedPixels.ReadUInt32());
				CHECK(glm::length(newRg - (glm::vec2{.25f, .5f} + oldRg * .5f)) < .005f);
				CHECK(glm::length(newBa - (glm::vec2{0, .5f} + oldBa * .5f)) < .005f);
			}
			REQUIRE(renderer.DropPortalImage(composed));
			accepted.Producer.World = "copied-lens-world";
			const std::array lensOwners{
				render::WorldContentOwner{core::Name(accepted.Producer.World), lensOwner}
			};
			accepted.Lenses.TimeSeconds = 7;
			for (const auto shader : {lensA, lensB, lensA}) {
				render::PortalCaptureLens lens;
				lens.Position = accepted.Camera.Position;
				lens.Orientation = {0, 0, 0, 1};
				lens.Shader = shader.Text();
				lens.ProgramHash = renderer.LensShaderHash(shader, lensOwner);
				lens.Radius = 100;
				lens.Strength = 3;
				accepted.Lenses.Entries.push_back(std::move(lens));
			}
			CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
			view.ForeignContentOwners = lensOwners;
			auto wrongHash = accepted;
			wrongHash.Lenses.Entries[0].ProgramHash =
				assets::Hasher::Of(std::as_bytes(std::span("wrong", 5)));
			CHECK(renderer.ComposePortalBodyImage(wrongHash, view) == 0);
			REQUIRE(renderer.ComposePortalBodyImage(accepted, view) != 0);
			const auto lensToken = renderer.QueueResourceImage(
				core::Name(
					seam ? "portal-body-seam-image/overlay/lenses/layers-2/0"
						 : "portal-body-eye-image/overlay/lenses/layers-2/0"
				),
				core::Name("export")
			);
			REQUIRE(lensToken != 0);
			const auto lensImage = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(lensImage != 0);
			const auto lensed = AwaitImage(renderer, lensToken);
			REQUIRE(lensed.Status == render::ResourceImageStatus::Ok);
			CHECK(lensed.Depth == blended.Depth);
			core::ByteReader beforeLens(blended.Pixels), afterLens(lensed.Pixels), lensDepth(blended.Depth);
			for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
				const auto beforeRg = glm::unpackHalf2x16(beforeLens.ReadUInt32());
				const auto beforeBa = glm::unpackHalf2x16(beforeLens.ReadUInt32());
				const auto afterRg = glm::unpackHalf2x16(afterLens.ReadUInt32());
				const auto afterBa = glm::unpackHalf2x16(afterLens.ReadUInt32());
				// Binary fractions keep HDR16 stores independent of nearest/zero rounding.
				const glm::vec3 addition{7.f / 16, lensDepth.ReadFloat() / 16, 3.f / 16};
				const auto stored = [](glm::vec3 value) {
					const auto redGreen = glm::unpackHalf2x16(glm::packHalf2x16({value.r, value.g}));
					const auto blue = glm::unpackHalf2x16(glm::packHalf2x16({value.b, 0}));
					return glm::vec3{redGreen.x, redGreen.y, blue.x};
				};
				glm::vec3 expected{beforeRg.x, beforeRg.y, beforeBa.x};
				expected = stored(expected * .5f + addition);
				expected = stored(expected * 2.f + glm::vec3(addition.z, addition.y, addition.x));
				expected = stored(expected * .5f + addition);
				CHECK(glm::length(glm::vec3(afterRg.x, afterRg.y, afterBa.x) - expected) < .01f);
				CHECK(afterBa.y == beforeBa.y);
			}
			REQUIRE(renderer.DropPortalImage(lensImage));
			accepted.Lenses = {};
			view.ForeignContentOwners = {};

			accepted.SpatialOverlayImage = imported[1];
			CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
			accepted.SpatialOverlayImage = 0;
			CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
			CHECK(renderer.DropPortalImage(overlayHandles[3]));
			CHECK_FALSE(renderer.PortalImageLayerSetReady(overlayHandles[0]));
			CHECK_FALSE(renderer.DropPortalImage(overlayHandles[0]));
			CHECK(renderer.PortalImageUsage().Images == baseline.Images);
			CHECK(renderer.PortalImageUsage().TextureBytes == baseline.TextureBytes);
			auto invalid = saved;
			invalid.SpatialOverlay->ContentRevision++;
			const auto before = overlayHandles;
			CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(invalid), overlayHandles));
			CHECK(overlayHandles == before);
			CHECK(renderer.PortalImageUsage().Images == baseline.Images);
		}
	}

	CHECK(renderer.DropPortalImage(view.EyeImage));
	for (const auto handle : view.EyeTransparentImages)
		CHECK_FALSE(renderer.DropPortalImage(handle));
	CHECK(renderer.PortalImageUsage().Images == 0);
}
