// Portal capture content: binding ownership, body motion, flipbook revisions and eye body selection.
#include "PortalImageRuntimeFixtures.hpp"

TEST_SUITE_ID("engine.render.portalimageruntimecontent")
TEST_DEPENDS("engine.render.portalimageruntime")

TEST_CASE("portal source refuses an image binding owned by another world index", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	auto binding = Binding();
	binding.World = worlds.Destination.Index;
	CHECK(source.Issue(worlds.Requests, Request(), binding, START).Status == PortalInboxStatus::Invalid);
	CHECK(worlds.Universe.PresentationQueueUsage().Messages == 0);
}

TEST_CASE("body motion follows the completed portal request", "[render][portal-runtime]") {
	RuntimeWorlds worlds;
	Renderer renderer;
	PortalImageSource source(worlds.Universe, renderer, worlds.Source, worlds.Replies);
	auto request = Request();
	PortalGeometry geometry;
	PortalGeometryRow body;
	body.Name = "Body";
	geometry.Rows.push_back(body);
	std::string error;
	REQUIRE(EncodePortalGeometry(geometry, request.Geometry, error));
	const auto first = source.Issue(worlds.Requests, request, Binding(), START);
	REQUIRE(first.Status == PortalInboxStatus::Issued);
	CHECK(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Busy);
	geometry.Rows[0].Pose[0] = 1;
	REQUIRE(EncodePortalGeometry(geometry, request.Geometry, error));
	CHECK(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Busy);
	PortalImageProducer producer(worlds.Universe, renderer, worlds.Destination, worlds.Requests);
	CHECK(producer.Pump(0, 0, START).Requests == 1);
	const auto replies = worlds.Universe.TakePresentation(worlds.Replies);
	REQUIRE(replies.size() == 1);
	PortalImageReply previous;
	REQUIRE(DecodePortalImageReply(replies[0].Payload, previous, error));
	REQUIRE(
		worlds.Universe.SendPresentation(
			worlds.Destination, worlds.Requests, worlds.Replies, first.RequestId, replies[0].Payload
		) == world::PresentationStatus::Ok
	);
	REQUIRE(source.Poll(START).size() == 1);
	CHECK(source.Issue(worlds.Requests, request, Binding(), START).Status == PortalInboxStatus::Busy);
	const auto messages = worlds.Universe.TakePresentation(worlds.Requests);
	REQUIRE(messages.size() == 1);
	// Poll immediately issues the retained latest body demand. A later caller
	// joins that request instead of creating a duplicate capture.
	CHECK(messages[0].Correlation != first.RequestId);
	PortalImageRequest current;
	REQUIRE(DecodePortalImageRequest(messages[0].Payload, current, error));
	CHECK(current.Key.RequestId != first.RequestId);
	CHECK(previous.Key.CameraRevision != current.Key.CameraRevision);
	CHECK(current.Geometry == request.Geometry);
}

TEST_CASE("portal content revision follows resident flipbook cells", "[render][gpu][portal-runtime][.]") {
	const bool throughHost = GENERATE(false, true);
	const bool spatialImage = GENERATE(false, true);
	CAPTURE(throughHost, spatialImage);
	const core::Name contentOwner("session:flipbook");
	RuntimeWorlds worlds;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	assets::TextureData texture;
	texture.Width = texture.Height = 2;
	texture.Format = assets::TextureFormat::RGBA8;
	texture.FlipbookSide = 2;
	texture.FlipbookFrames = 2;
	texture.FlipbookFrameRate = 2;
	texture.Pixels = {
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255}
	};
	REQUIRE(fixture.Render.AddTexture(core::Name("portal-flipbook"), texture, contentOwner));
	ecs::Entity wallEntity, pictureEntity;
	worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc part;
		part.Frame.Position = {0, 0, -4};
		part.Size = {8, 8, .1f};
		part.Simulated = false;
		const auto wall = scene::MakePart(store, part);
		wallEntity = wall;
		REQUIRE(store.SetParent(wall, workspace));
		auto appearance = *store.Get<scene::SurfaceAppearance>(wall);
		appearance.ColourMap = core::Name("portal-flipbook");
		store.Set(wall, appearance);
		if (spatialImage) {
			auto visual = *store.Get<scene::Visual>(wall);
			visual.Transparency = 1;
			store.Set(wall, visual);
			const auto canvas = store.CreateInstance(gui::GuiClass("SurfaceGui"), "ScopedCanvas");
			REQUIRE(store.SetParent(canvas, wall));
			gui::Surface surface;
			surface.On = gui::Face::Back;
			surface.CanvasSize = {100, 100};
			store.Set(canvas, surface);
			const auto label = store.CreateInstance(gui::GuiClass("ImageLabel"), "ScopedPicture");
			pictureEntity = label;
			REQUIRE(store.SetParent(label, canvas));
			gui::Element element;
			element.Size = {1, 0, 1, 0};
			store.Set(label, element);
			gui::Background background;
			background.Transparency = 1;
			background.BorderSizePixel = 0;
			store.Set(label, background);
			gui::Picture picture;
			picture.Image = core::Name("portal-flipbook");
			store.Set(label, picture);
		}
	});
	std::unique_ptr<PortalImageProducer> producer;
	std::unique_ptr<PortalImageHost> host;
	if (throughHost) {
		REQUIRE(worlds.Universe.ClosePresentation(worlds.Requests) == world::PresentationStatus::Ok);
		host = std::make_unique<PortalImageHost>(worlds.Universe, fixture.Render, nullptr, false);
		host->SetContentOwner(worlds.Destination, contentOwner);
		worlds.Requests = host->Serve(worlds.Destination);
		REQUIRE(worlds.Requests.Generation != 0);
	} else {
		producer = std::make_unique<PortalImageProducer>(
			worlds.Universe, fixture.Render, worlds.Destination, worlds.Requests, nullptr, nullptr, false
		);
		producer->SetContentOwner(contentOwner);
	}
	const auto bindOwner = [&](core::Name owner) {
		if (host)
			host->SetContentOwner(worlds.Destination, owner);
		else
			producer->SetContentOwner(owner);
	};
	uint64_t sequence = 0;
	const auto capture = [&](std::optional<PortalImageVersion> known = {}, bool *renewed = nullptr) {
		if (renewed) *renewed = false;
		auto request = Request();
		if (spatialImage) request.Scope = PortalImageScope::CompleteWorld;
		request.Key.RequestId = ++sequence;
		request.KnownImage = known;
		std::vector<std::byte> wire;
		std::string error;
		REQUIRE(EncodePortalImageRequest(request, wire, error));
		REQUIRE(
			worlds.Universe.SendPresentation(
				worlds.Source, worlds.Replies, worlds.Requests, sequence, wire
			) == world::PresentationStatus::Ok
		);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (host)
				host->Pump(0, 0, START);
			else
				producer->Pump(0, 0, START);
			auto replies = worlds.Universe.TakePresentation(worlds.Replies);
			if (replies.empty()) {
				SDL_Delay(1);
				continue;
			}
			REQUIRE(replies.size() == 1);
			PortalImageReply reply;
			PortalResidentReceipt renewal;
			if (renewed && DecodePortalImageRenewal(replies[0].Payload, renewal, error)) {
				*renewed = true;
				return reply;
			}
			REQUIRE(DecodePortalImageReply(replies[0].Payload, reply, error));
			REQUIRE(reply.Status == PortalImageStatus::Ok);
			return reply;
		}
		FAIL("portal capture timed out");
		return PortalImageReply{};
	};
	worlds.Universe.Tick(.01f);
	(void)capture();
	const auto first = capture();
	worlds.Universe.Tick(.1f);
	const auto sameCell = capture();
	CHECK(sameCell.PixelHash == first.PixelHash);
	CHECK(sameCell.ContentRevision == first.ContentRevision);
	SECTION("authored opaque discard preserves nearer and background depth") {
		if (spatialImage) return;
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			store.GetMutable<scene::Visual>(wallEntity)->Transparency = 1;
			for (const bool near : {false, true}) {
				scene::PartDesc part;
				part.Frame.Position = near ? core::Vector3{2, 0, -2} : core::Vector3{0, 0, -6};
				part.Size = near ? core::Vector3{1, 8, .1f} : core::Vector3{16, 16, .1f};
				part.Simulated = false;
				const auto entity = scene::MakePart(store, part);
				REQUIRE(store.SetParent(entity, store.ParentOf(wallEntity)));
				store.GetMutable<scene::Visual>(entity)->Tint =
					near ? core::Color3{1, 0, 0} : core::Color3{0, 1, 0};
			}
		});
		const auto background = capture();
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			const core::Name name("capture.cutout");
			const auto shader = store.CreateInstance(scene::ShaderScriptClass(), name.Text());
			REQUIRE(
				scene::SetShaderSource(
					store,
					shader,
					"#version 450\nlayout(location=0) out vec4 colour;\nvoid "
					"main(){if(gl_FragCoord.x<4)discard;colour=vec4(0,0,1,1);}"
				)
			);
			const auto material = store.CreateInstance(scene::MaterialClass(), "CutoutMaterial");
			REQUIRE(store.SetParent(material, wallEntity));
			store.GetMutable<scene::MaterialRef>(material)->Shader = name;
			store.GetMutable<scene::Visual>(wallEntity)->Transparency = 0;
			scene::ResolveMaterials(store);
		});
		const auto cutout = capture();
		REQUIRE(cutout.Width == 8);
		REQUIRE(cutout.Height == 8);
		REQUIRE(cutout.Depth.size() == 8 * 8 * sizeof(float));
		REQUIRE(background.Depth.size() == cutout.Depth.size());
		for (const uint32_t x : {1u, 5u, 7u}) {
			CAPTURE(x);
			const size_t colour = 4 * cutout.RowStride + x * 8;
			core::ByteReader cutoutColour(std::span(cutout.Pixels).subspan(colour, 8));
			core::ByteReader referenceColour(std::span(background.Pixels).subspan(colour, 8));
			const auto cutoutRg = glm::unpackHalf2x16(cutoutColour.ReadUInt32());
			const auto cutoutBa = glm::unpackHalf2x16(cutoutColour.ReadUInt32());
			const auto referenceRg = glm::unpackHalf2x16(referenceColour.ReadUInt32());
			const auto referenceBa = glm::unpackHalf2x16(referenceColour.ReadUInt32());
			CAPTURE(cutoutRg.x, cutoutRg.y, cutoutBa.x, referenceRg.x, referenceRg.y, referenceBa.x);
			core::ByteReader foregroundDepth(std::span(cutout.Depth).subspan((4 * 8 + x) * 4, 4));
			core::ByteReader backgroundDepth(std::span(background.Depth).subspan((4 * 8 + x) * 4, 4));
			const float foreground = foregroundDepth.ReadFloat(), behind = backgroundDepth.ReadFloat();
			if (x == 5) {
				CHECK(cutoutRg.x == 0);
				CHECK(cutoutRg.y == 0);
				CHECK(cutoutBa.x == 1);
				CHECK(foreground > 0);
				CHECK(foreground < behind);
			} else {
				// Adding the wall changes shadows and ambient occlusion, but the
				// visible material and its exact depth must survive the cutout.
				const float dominant = x == 1 ? cutoutRg.y : cutoutRg.x;
				const float other = x == 1 ? cutoutRg.x : cutoutRg.y;
				CHECK(dominant > .01f);
				CHECK(dominant > 4 * other);
				CHECK(dominant > 4 * cutoutBa.x);
				CHECK(foreground == behind);
			}
		}
	}
	SECTION("producer respects disabled postprocessing") {
		const core::Name grade("capture.disabled-grade");
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			const auto shader = store.CreateInstance(scene::ShaderScriptClass(), grade.Text());
			REQUIRE(
				scene::SetShaderSource(
					store,
					shader,
					"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(0,1,0,1);}"
				)
			);
			scene::SetPostProcessShader(store, grade);
		});
		const auto unchanged = capture();
		CHECK_FALSE(fixture.Render.PostProcessShaderName(contentOwner).IsValid());
		CHECK_FALSE(fixture.Render.HasShader(grade, contentOwner));
		CHECK(unchanged.PixelHash == sameCell.PixelHash);
		CHECK(unchanged.ContentRevision == sameCell.ContentRevision);
	}
	SECTION("authored material and spatial shaders invalidate capture renewal") {
		const core::Name name("capture.authored");
		ecs::Entity shader;
		const auto code = [&](const char *colour) {
			return std::string("#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=") +
				   colour + ";}";
		};
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			shader = store.CreateInstance(scene::ShaderScriptClass(), name.Text());
			REQUIRE(scene::SetShaderSource(store, shader, code("vec4(1,0,0,1)")));
			if (spatialImage)
				store.GetMutable<gui::Picture>(pictureEntity)->Shader = name;
			else {
				const auto material = store.CreateInstance(scene::MaterialClass(), "CaptureMaterial");
				REQUIRE(store.SetParent(material, wallEntity));
				store.GetMutable<scene::MaterialRef>(material)->Shader = name;
				scene::ResolveMaterials(store);
			}
		});
		const auto red = capture();
		CHECK(fixture.Render.HasShader(name, contentOwner) == !spatialImage);
		const auto resourceRevision = fixture.Render.ResourceRevision();
		bool renewed = false;
		capture(PortalImageVersion{red.ContentRevision, red.LightingRevision}, &renewed);
		CHECK(renewed);
		CHECK(fixture.Render.ResourceRevision() == resourceRevision);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			REQUIRE(scene::SetShaderSource(store, shader, code("vec4(0,1,0,1)")));
		});
		const auto green = capture(PortalImageVersion{red.ContentRevision, red.LightingRevision});
		CHECK(green.PixelHash != red.PixelHash);
		CHECK(green.ContentRevision != red.ContentRevision);
		if (spatialImage) CHECK(fixture.Render.ResourceRevision() == resourceRevision);
		capture(PortalImageVersion{green.ContentRevision, green.LightingRevision}, &renewed);
		CHECK(renewed);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			REQUIRE(scene::SetShaderSource(store, shader, "not a shader"));
		});
		capture(PortalImageVersion{green.ContentRevision, green.LightingRevision}, &renewed);
		CHECK(renewed);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) { store.Destroy(shader); });
		const auto removed = capture(PortalImageVersion{green.ContentRevision, green.LightingRevision});
		CHECK(removed.PixelHash != green.PixelHash);
		CHECK(removed.ContentRevision != green.ContentRevision);
	}
	SECTION("editable image changes are prepared before capture renewal") {
		ecs::Entity editable;
		core::Name editableName;
		const core::Name editableOwner = worlds.Universe.NameOf(worlds.Destination);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			editable = store.CreateInstance(scene::EditableImageClass(), "capture image");
			scene::EditableImage image;
			image.Width = image.Height = 2;
			image.Pixels.assign(16, 255);
			store.Set(editable, image);
			editableName = scene::EditableImageContentName(store, editable);
			store.GetMutable<scene::SurfaceAppearance>(wallEntity)->ColourMap = editableName;
			if (spatialImage) store.GetMutable<gui::Picture>(pictureEntity)->Image = editableName;
		});
		const auto prepared = capture();
		REQUIRE(fixture.Render.TextureHandle(editableName, editableOwner) != nullptr);
		CHECK(fixture.Render.TextureHandle(editableName, contentOwner) == nullptr);
		CHECK(fixture.Render.TextureHandle(editableName) == nullptr);
		const auto uploadedRevision = fixture.Render.ResourceRevision();
		const auto unchanged = capture();
		CHECK(fixture.Render.ResourceRevision() == uploadedRevision);
		CHECK(unchanged.PixelHash == prepared.PixelHash);
		CHECK(unchanged.ContentRevision == prepared.ContentRevision);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			auto *image = store.GetMutable<scene::EditableImage>(editable);
			for (size_t at = 0; at < image->Pixels.size(); at += 4) {
				image->Pixels[at] = image->Pixels[at + 1] = 0;
			}
			++image->Revision;
		});
		const auto changed = capture(PortalImageVersion{prepared.ContentRevision, prepared.LightingRevision});
		CHECK(changed.PixelHash != prepared.PixelHash);
		CHECK(changed.ContentRevision != prepared.ContentRevision);
		CHECK(fixture.Render.ResourceRevision() > uploadedRevision);
		const core::Name reboundOwner("session:editable-rebound");
		bindOwner(reboundOwner);
		const auto rebound = capture();
		CHECK(fixture.Render.TextureHandle(editableName, editableOwner) != nullptr);
		CHECK(fixture.Render.TextureHandle(editableName, reboundOwner) == nullptr);
		CHECK(rebound.PixelHash == changed.PixelHash);
		bindOwner(contentOwner);
		const auto restored = capture();
		CHECK(restored.PixelHash == changed.PixelHash);
		const auto beforeClear = fixture.Render.ResourceRevision();
		if (host) {
			host->RemoveWorld(worlds.Destination);
			host->SetContentOwner(worlds.Destination, contentOwner);
			worlds.Requests = host->Serve(worlds.Destination);
			REQUIRE(worlds.Requests.Generation != 0);
		} else {
			producer->Clear();
		}
		const auto reopened = capture();
		CHECK(reopened.PixelHash == changed.PixelHash);
		CHECK(fixture.Render.ResourceRevision() > beforeClear);
	}
	SECTION("editable mesh changes are prepared before capture renewal") {
		if (spatialImage) return;
		ecs::Entity editable;
		core::Name editableName;
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			editable = store.CreateInstance(scene::EditableMeshClass(), "capture mesh");
			REQUIRE(scene::AddVertex(store, editable, {-.5f, -.5f, 0}));
			REQUIRE(scene::AddVertex(store, editable, {.5f, -.5f, 0}));
			REQUIRE(scene::AddVertex(store, editable, {0, .5f, 0}));
			REQUIRE(scene::AddTriangle(store, editable, 0, 1, 2));
			REQUIRE(scene::AddTriangle(store, editable, 2, 1, 0));
			editableName = scene::EditableMeshContentName(store, editable);
			store.GetMutable<scene::Visual>(wallEntity)->Mesh = editableName;
		});
		const auto prepared = capture();
		core::Vector3 extent;
		REQUIRE(fixture.Render.MeshExtentOf(editableName, extent, contentOwner));
		CHECK(extent.X == .5f);
		CHECK_FALSE(fixture.Render.MeshExtentOf(editableName, extent));
		CHECK(prepared.PixelHash != sameCell.PixelHash);
		const auto uploadedRevision = fixture.Render.ResourceRevision();
		const auto unchanged = capture();
		CHECK(fixture.Render.ResourceRevision() == uploadedRevision);
		CHECK(unchanged.PixelHash == prepared.PixelHash);
		CHECK(unchanged.ContentRevision == prepared.ContentRevision);
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			auto *mesh = store.GetMutable<scene::EditableMesh>(editable);
			// Keep the bounds fixed: renderer fits the mesh to the part's authored size.
			mesh->Positions[2].X = .375f;
			++mesh->Revision;
		});
		const auto changed = capture(PortalImageVersion{prepared.ContentRevision, prepared.LightingRevision});
		CHECK(changed.PixelHash != prepared.PixelHash);
		CHECK(changed.ContentRevision != prepared.ContentRevision);
		REQUIRE(fixture.Render.MeshExtentOf(editableName, extent, contentOwner));
		CHECK(extent.X == .5f);
		CHECK(fixture.Render.ResourceRevision() > uploadedRevision);
	}
	SECTION("same-name texture replacement") {
		for (size_t at = 0; at < texture.Pixels.size(); at += 4) {
			texture.Pixels[at] = std::byte{0};
			texture.Pixels[at + 1] = std::byte{0};
			texture.Pixels[at + 2] = std::byte{255};
		}
		const auto beforeReplacement = fixture.Render.ResourceRevision();
		REQUIRE(fixture.Render.AddTexture(core::Name("portal-flipbook"), texture, contentOwner));
		CHECK(fixture.Render.ResourceRevision() > beforeReplacement);
		const auto replaced =
			capture(PortalImageVersion{sameCell.ContentRevision, sameCell.LightingRevision});
		CHECK(replaced.PixelHash != sameCell.PixelHash);
		CHECK(replaced.ContentRevision != sameCell.ContentRevision);
		const auto beforeRefusal = fixture.Render.ResourceRevision();
		CHECK_FALSE(fixture.Render.DropTexture(core::Name("not-registered")));
		CHECK(fixture.Render.ResourceRevision() == beforeRefusal);
		REQUIRE(fixture.Render.DropTexture(core::Name("portal-flipbook"), contentOwner));
		CHECK(fixture.Render.ResourceRevision() > beforeRefusal);
		const auto missing = capture();
		CHECK(missing.ContentRevision != replaced.ContentRevision);
		REQUIRE(fixture.Render.AddTexture(core::Name("portal-flipbook"), texture, contentOwner));
		const auto restored = capture();
		CHECK(restored.PixelHash == replaced.PixelHash);
		CHECK(restored.ContentRevision != missing.ContentRevision);
	}
	SECTION("content owner changes invalidate captures without changing assets") {
		const core::Name other("session:other-flipbook");
		for (size_t at = 0; at < texture.Pixels.size(); at += 4) {
			texture.Pixels[at] = std::byte{0};
			texture.Pixels[at + 1] = std::byte{0};
			texture.Pixels[at + 2] = std::byte{255};
		}
		REQUIRE(fixture.Render.AddTexture(core::Name("portal-flipbook"), texture, other));
		const auto before = capture();
		CHECK(before.PixelHash == sameCell.PixelHash);
		const auto resourceRevision = fixture.Render.ResourceRevision();
		bindOwner(other);
		CHECK(fixture.Render.ResourceRevision() == resourceRevision);
		const auto switched = capture(PortalImageVersion{before.ContentRevision, before.LightingRevision});
		CHECK(switched.PixelHash != before.PixelHash);
		CHECK(switched.ContentRevision != before.ContentRevision);
		bindOwner(contentOwner);
		const auto restored = capture();
		CHECK(restored.PixelHash == before.PixelHash);
		CHECK(restored.ContentRevision == before.ContentRevision);
	}
	SECTION("live instance pictures use the world namespace while assets use a content alias") {
		if (!spatialImage) return;
		const core::Name owner = worlds.Universe.NameOf(worlds.Destination);
		REQUIRE(owner != contentOwner);
		const core::Name graphName("Signal"), imageName("imagegraph-instance://Signal#image");
		imagegraph::Document graph{
			.Nodes = {{"blue", imagegraph::Solid{1, 1, {0, 0, 255, 255}}, {}, {}}},
			.Outputs = {{"image", "blue"}},
			.Parameters = {},
			.Bindings = {}
		};
		imagegraph::Diagnostic diagnostic;
		REQUIRE(fixture.Render.SetImageGraph(owner, graphName, graph, {}, diagnostic));
		REQUIRE(
			fixture.Render.EvaluateImageGraph(owner, graphName, "image", imageName, diagnostic) ==
			ImageGraphEvaluation::Updated
		);
		assets::TextureData wrongNamespace;
		wrongNamespace.Width = wrongNamespace.Height = 1;
		wrongNamespace.Pixels = {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
		REQUIRE(fixture.Render.AddTexture(imageName, wrongNamespace, contentOwner));
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			store.GetMutable<gui::Picture>(pictureEntity)->Image = imageName;
		});
		const auto captured = capture();
		REQUIRE(captured.Width == 8);
		REQUIRE(captured.Height == 8);
		core::ByteReader pixel(std::span(captured.Pixels).subspan(4 * captured.RowStride + 4 * 8, 8));
		const auto redGreen = glm::unpackHalf2x16(pixel.ReadUInt32());
		const auto blueAlpha = glm::unpackHalf2x16(pixel.ReadUInt32());
		CHECK(redGreen.x < .01f);
		CHECK(redGreen.y < .01f);
		CHECK(blueAlpha.x > .5f);
		CHECK(blueAlpha.y > .99f);
	}
	SECTION("retiring a hosted world removes its content binding") {
		if (host) {
			host->RemoveWorld(worlds.Destination);
			worlds.Requests = host->Serve(worlds.Destination);
			REQUIRE(worlds.Requests.Generation != 0);
			const auto unbound = capture();
			CHECK(unbound.PixelHash != sameCell.PixelHash);
			bindOwner(contentOwner);
			const auto restored = capture();
			CHECK(restored.PixelHash == sameCell.PixelHash);
		}
	}

	SECTION("flipbook advancement") {
		for (int frame = 0; frame < 36; ++frame)
			worlds.Universe.Tick(1.0f / 60.0f);
		const auto nextCell = capture(PortalImageVersion{first.ContentRevision, first.LightingRevision});
		worlds.Universe.Enter(worlds.Destination, [&](ecs::Store &store) {
			CHECK(store.Time().Elapsed >= .5);
		});
		CHECK(nextCell.PixelHash != first.PixelHash);
		CHECK(nextCell.ContentRevision != first.ContentRevision);
	}
}

TEST_CASE(
	"copied whole-eye images preserve player body selection", "[render][gpu][portal-runtime][eye-body][.]"
) {
	const bool foreignBody = GENERATE(false, true);
	RuntimeWorlds worlds;
	std::vector<std::byte> geometry;
	scene::RegisterSceneClasses();
	worlds.Universe.Enter(worlds.Destination, [](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc wall;
		wall.Frame.Position = {0, 0, -16};
		wall.Size = {40, 40, .1f};
		const auto backdrop = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(backdrop, workspace));
		store.GetMutable<scene::Visual>(backdrop)->Tint = {.2f, 0, 0};
		const auto missing = scene::MakePart(store, wall);
		store.GetMutable<scene::Visual>(missing)->Mesh = core::Name("unloaded-before-body");
		scene::PoseCharacters(store);
		scene::CapturePreviousTransforms(store);
		store.SetResource(scene::Sun{{0, 0, 1}, {.5f, .5f, .5f}});
	});
	worlds.Universe.Enter(foreignBody ? worlds.Source : worlds.Destination, [&](ecs::Store &store) {
		if (foreignBody) scene::InstallServices(store);
		const auto player = scene::AddPlayer(store, "viewer", false, 91);
		scene::CharacterDesc character;
		character.Frame.Position = {0, -2.5f, -10};
		character.TorsoColour = character.LegColour = character.SkinColour = {0, 0, 1};
		const auto model = scene::MakeCharacter(store, character);
		REQUIRE(scene::SetPlayerCharacter(store, player, model));
		scene::PoseCharacters(store);
		scene::CapturePreviousTransforms(store);
		if (foreignBody) {
			RegisterPresentationComponents();
			store.SetResource(DrawList{});
			scene::SyncRendered(store);
			CollectInstances(store);
			const auto *draws = store.Resource<DrawList>();
			REQUIRE(draws != nullptr);
			std::string error;
			REQUIRE(EncodePortalDraws(store, draws->Instances, draws->JointFrames, geometry, error));
			store.DestroyInstance(model);
		}
	});
	render::test::FixtureDevice sourceDevice, destinationDevice;
	sourceDevice.Initialise();
	destinationDevice.Initialise();
	REQUIRE(sourceDevice.Render.Backend().Device != destinationDevice.Render.Backend().Device);
	PortalImageSource source(worlds.Universe, sourceDevice.Render, worlds.Source, worlds.Replies);
	PortalImageProducer producer(
		worlds.Universe, destinationDevice.Render, worlds.Destination, worlds.Requests
	);
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultEyeDocument(), pipeline, offender) == graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipelineName("copied.body-eye");
	REQUIRE(sourceDevice.Render.SetPipeline(pipelineName, pipeline));
	SceneTarget target{65, 65};
	View eye;
	eye.Target = &target;
	eye.World = worlds.Source.Index;
	eye.WorldName = core::Name("portal-source");
	eye.Pipeline = pipelineName;
	eye.EyeImageKey = core::Name("Door");
	const auto started = std::chrono::steady_clock::now();
	const auto now = [&] { return START + (std::chrono::steady_clock::now() - started); };
	for (const bool hidden : {false, true, false}) {
		CAPTURE(hidden);
		eye.EyePlayer = hidden ? std::optional<int64_t>(91) : std::nullopt;
		PortalImageDemand demand;
		auto &request = demand.Request;
		REQUIRE(
			BuildPortalEyeDemand(
				eye.EyeImageKey,
				eye,
				{.Width = 65, .Height = 65, .RecursionDepth = 0, .PixelBudget = 65 * 65},
				demand
			) == PortalDemandStatus::Ready
		);
		request.Geometry = geometry;
		REQUIRE(
			source.Issue(worlds.Requests, request, demand.Binding, now()).Status == PortalInboxStatus::Issued
		);
		CHECK(source.CurrentImage("Door") == 0);
		std::vector<PortalRuntimeCompletion> completed;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (completed.empty() && std::chrono::steady_clock::now() < deadline) {
			const auto observed = now();
			producer.Pump(0, 1, observed);
			completed = source.Poll(observed);
			if (completed.empty()) SDL_Delay(1);
		}
		REQUIRE(completed.size() == 1);
		INFO(completed[0].Diagnostic);
		REQUIRE(completed[0].Status == PortalImageStatus::Ok);
		eye.EyeImage = source.CurrentImage("Door");
		REQUIRE(eye.EyeImage != 0);
		const auto uploadedBefore = sourceDevice.Render.PortalImageUsage().UploadedBytes;
		OverlayImage overlay;
		sourceDevice.Render.Render(std::span(&eye, 1), overlay, nullptr, false);
		CHECK(sourceDevice.Render.PortalImageUsage().UploadedBytes > uploadedBefore);
		CHECK(sourceDevice.Render.PortalImageUsage().PendingCpuBytes == 0);
		const auto image = render::test::CaptureResource(
			sourceDevice.Render,
			core::Name("composed-image"),
			0,
			65,
			65,
			render::test::ImageFormat::Bgra8Unorm
		);
		render::test::WriteImagePreview(
			core::Paths::Base() / (std::string("eye-import-") + (foreignBody ? "foreign-" : "native-") +
								   (hidden ? "hidden" : "visible") + ".ppm"),
			image.View()
		);
		const auto *centre = image.Bytes.data() + 32 * image.RowStrideBytes + 32 * 4;
		const auto blue = std::to_integer<int>(centre[0]), red = std::to_integer<int>(centre[2]);
		CAPTURE(red, blue);
		CHECK((hidden ? red > blue : blue > red));
	}
}
